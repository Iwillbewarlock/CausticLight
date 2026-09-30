#include "PCH.h"

#include "DrawPass.h"
#include "Settings.h"

// Bytecode arrays generated at build time by fxc (CMakeLists.txt) from shaders/CausticPass.hlsl.
#include "CausticPassPS.h"
#include "CausticPassVS.h"
#include "CausticCurvPS.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace cl::DrawPass
{
	namespace
	{
		using math::kMaxDrawZones;
		using math::WaterMask;
		using math::ZoneGpuInput;

		constexpr const char* kCalmAtlasPath = "textures\\CausticLight\\calm.dds";
		// Read by the plugin itself (the engine's loader handed out a 512x512 copy of the 2048 atlas).
		// calm, normal, rough (the floor's refraction in alpha), then the walls' reflection screens of each (RGB)
		constexpr const char* kAtlasFiles[6]{ "Data\\textures\\CausticLight\\calm.dds", "Data\\textures\\CausticLight\\normal.dds",
			"Data\\textures\\CausticLight\\rough.dds", "Data\\textures\\CausticLight\\refl_calm.dds", "Data\\textures\\CausticLight\\refl_normal.dds",
			"Data\\textures\\CausticLight\\refl_rough.dds" };
		constexpr int         kMaxTextureAttempts = 600;  // task ticks (~60 s: the loader may hand out a placeholder first)

		template <class T>
		void Release(T*& p)
		{
			if (p) {
				p->Release();
				p = nullptr;
			}
		}

		// ---- resources (created once from the task) ------------------------------------

		ID3D11VertexShader*      g_vs{ nullptr };
		ID3D11PixelShader*       g_ps{ nullptr };
		ID3D11Buffer*            g_cb{ nullptr };
		ID3D11BlendState*        g_blend{ nullptr };
		ID3D11RasterizerState*   g_raster{ nullptr };
		ID3D11DepthStencilState* g_depthOff{ nullptr };
		ID3D11SamplerState*      g_sampler{ nullptr };
		bool                     g_created{ false };
		bool                     g_createFailed{ false };
		bool                     g_hookInstalled{ false };

		// Kept for the whole process and deliberately never destroyed: no engine release runs at
		// DLL unload. The SRV is re-read from it every frame (a reloaded texture swaps it).
		auto&                     g_atlas = *new RE::NiPointer<RE::NiTexture>();
		ID3D11ShaderResourceView* g_atlasSRV{ nullptr };
		std::array<ID3D11ShaderResourceView*, 6> g_own{};  // calm, normal, rough, refl_calm, refl_normal, refl_rough, read from the files
		bool                                     g_ownTried{ false };
		int                       g_textureAttempts{ 0 };
		bool                      g_textureGaveUp{ false };

		// Water mask textures, one per GPU slot, rebuilt when the slot's mask changes (rare: a zone
		// becomes active or an automatic zone is re-scanned). The shared_ptr keeps the CPU data alive
		// while its texture is in use.
		struct MaskTexture
		{
			std::shared_ptr<const WaterMask> mask;
			ID3D11ShaderResourceView*        srv{ nullptr };
		};
		std::array<MaskTexture, kMaxDrawZones> g_maskTex{};
		std::uint32_t                          g_maskFailures{ 0 };

		ID3D11ShaderResourceView* MaskSRV(ID3D11DeviceContext* ctx, std::size_t slot, const std::shared_ptr<const WaterMask>& m)
		{
			auto& t = g_maskTex[slot];
			if (t.mask == m) {
				return t.srv;
			}
			if (t.srv) {
				t.srv->Release();
				t.srv = nullptr;
			}
			t.mask = m;
			if (!m || m->size <= 0 || m->mips.empty()) {
				return nullptr;
			}
			ID3D11Device* dev = nullptr;
			ctx->GetDevice(&dev);
			if (!dev) {
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = static_cast<UINT>(m->size);
			td.MipLevels = static_cast<UINT>(m->mips.size());
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // r: water, g/b/a: water light 0/1/2 reaches (a copy of r when not known)
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			std::vector<std::vector<std::uint8_t>> packed(m->mips.size());
			std::vector<D3D11_SUBRESOURCE_DATA>    init(m->mips.size());
			for (std::size_t l = 0; l < m->mips.size(); ++l) {
				const auto& r = m->mips[l];
				packed[l].resize(r.size() * 4);
				for (std::size_t k = 0; k < 3; ++k) {
					const auto& g = m->litMips[k].size() == m->mips.size() ? m->litMips[k][l] : r;
					for (std::size_t i = 0; i < r.size(); ++i) {
						packed[l][4 * i + 1 + k] = g[i];
					}
				}
				for (std::size_t i = 0; i < r.size(); ++i) {
					packed[l][4 * i] = r[i];
				}
				init[l] = { packed[l].data(), static_cast<UINT>(std::max(1, m->size >> l) * 4), 0 };
			}
			ID3D11Texture2D* tex = nullptr;
			HRESULT          hr = dev->CreateTexture2D(&td, init.data(), &tex);
			if (SUCCEEDED(hr)) {
				hr = dev->CreateShaderResourceView(tex, nullptr, &t.srv);
				tex->Release();
			}
			dev->Release();
			if (FAILED(hr) && g_maskFailures++ < 5) {
				logger::error("draw pass: water mask texture for slot {} failed (0x{:08X})", slot, static_cast<std::uint32_t>(hr));
			}
			return t.srv;
		}

		// ---- staging (task writes, hooks read; same thread) --------------------------------

		std::array<ZoneGpuInput, kMaxDrawZones> g_zones{};
		std::array<bool, kMaxDrawZones>         g_used{};
		std::array<float, kMaxDrawZones>        g_clock{};

		// ---- first-frame diagnostics (Render fills, the task logs) -------------------------

		struct Diagnostics
		{
			bool        ready{ false };
			bool        logged{ false };
			UINT        mainWidth{ 0 }, mainHeight{ 0 };
			DXGI_FORMAT mainRtvFormat{ DXGI_FORMAT_UNKNOWN };
			bool        boundIsMain{ false };
			bool        boundDsv{ false };
			const char* depthSource{ "none" };
			UINT        depthWidth{ 0 }, depthHeight{ 0 };
			DXGI_FORMAT depthSrvFormat{ DXGI_FORMAT_UNKNOWN };
			DXGI_FORMAT depthTexFormat{ DXGI_FORMAT_UNKNOWN };
			D3D11_SRV_DIMENSION depthDim{ D3D11_SRV_DIMENSION_UNKNOWN };
			D3D11_VIEWPORT      viewport{};
			float               vpDiff{ 0.0f };  // max |viewMat*projMat - viewProjMat|
			int                 shadowRT[3]{ -1, -1, -1 };  // the engine's pending targets (RENDER_TARGET ids)
			int                 shadowDepth{ -1 };
			bool                invertible{ true };
		};
		Diagnostics g_diag;

		enum Skip : std::uint32_t
		{
			kSkipNoMainRTV = 1u << 0,
			kSkipNoDepthSRV = 1u << 1,
			kSkipNoViewport = 1u << 2,
			kSkipSmallViewport = 1u << 3,
			kSkipDepthNotTexture2D = 1u << 4,
			kSkipNoAtlas = 1u << 5,
			kSkipSingularCamera = 1u << 6,
			kSkipMapFailed = 1u << 7,
			kSkipSecondCall = 1u << 8,
		};
		std::uint32_t g_skipSeen{ 0 };
		std::uint32_t g_skipLogged{ 0 };
		std::uint32_t g_framesDrawn{ 0 };
		bool          g_firstDrawLogged{ false };

		// Draw at most once per frame. A frame ends when the per-frame hook calls BeginFrame OR
		// the engine's frame counter moves. If neither happens for kUnlockAfter post-opaque calls
		// the lock is released anyway (and that is logged once), so a missing frame signal can
		// never switch the pass off for good.
		constexpr int kUnlockAfter = 30;
		bool          g_drewThisFrame{ false };
		std::uint32_t g_drawEngineFrame{ 0 };
		int           g_blockedCalls{ 0 };
		std::uint32_t g_fallbackUnlocks{ 0 };
		bool          g_fallbackUnlockLogged{ false };

		// Frame-signal statistics over the first kStatFrames frames (logged once).
		constexpr std::uint32_t kStatFrames = 300;
		std::uint32_t           g_beginFrames{ 0 };
		std::uint32_t           g_postCalls{ 0 };
		std::uint32_t           g_engineFrameAtStart{ 0 };
		bool                    g_statsReady{ false };
		bool                    g_statsLogged{ false };
		std::uint32_t           g_statCalls{ 0 };
		std::uint32_t           g_statEngine{ 0 };

		// Depth source: every change is logged by the task.
		ID3D11ShaderResourceView* g_depthSrvNow{ nullptr };
		const char*               g_depthNameNow{ "none" };
		const char*               g_depthNamePrev{ "none" };
		std::uint32_t             g_depthChanges{ 0 };
		std::uint32_t             g_depthChangesLogged{ 0 };

		// Identity of the first call that actually drew (logged once).
		struct FirstDraw
		{
			bool           ready{ false };
			bool           logged{ false };
			std::uintptr_t boundRT0{ 0 };
			std::uintptr_t mainRTV{ 0 };
			UINT           width{ 0 }, height{ 0 };
			D3D11_VIEWPORT viewport{};
			std::uint32_t  engineFrame{ 0 };
			int            zones{ 0 };
			const char*    depth{ "none" };
		};
		FirstDraw g_firstDraw;

		bool g_enabled{ true };

		// Per-second statistics (LogSecond reads and resets them).
		struct SecondStats
		{
			std::uint32_t frames{ 0 };       // BeginFrame calls
			std::uint32_t framesDrawn{ 0 };  // frames with at least one zone drawn
			std::uint32_t zoneDraws{ 0 };    // zone draws in total
			std::uint64_t pixels{ 0 };       // scissor area in total
			int           lastZones{ 0 };
			std::uint64_t lastPixels{ 0 };
			float         lastViewportPixels{ 0.0f };
			std::array<math::PixelRect, kMaxDrawZones> lastRects{};
		};
		SecondStats g_second;

		// GPU time of our draws: timestamp queries in a ring of 4 frames, read back without stalling
		// (D3D11_ASYNC_GETDATA_DONOTFLUSH) a few frames later. Logged per second by LogSecond.
		struct GpuTimer
		{
			static constexpr int kRing = 4;
			ID3D11Query*         disjoint[kRing]{};
			ID3D11Query*         begin[kRing]{};
			ID3D11Query*         end[kRing]{};
			bool                 pending[kRing]{};
			int                  next{ 0 };
			bool                 created{ false };
			bool                 failed{ false };
			double               msSum{ 0.0 };
			double               msMax{ 0.0 };
			std::uint32_t        samples{ 0 };
		};
		GpuTimer g_timer;

		// Probe for Surface Tides (Nexus 191955): its water shaders read the simulated height field from
		// t96 (current) / t97 (previous) and its patch constants from b11 (Shaders/SurfaceTides/SurfaceField.hlsli).
		// Logged: whether those slots hold anything at our post-opaque point, on which stages, and what.
		// Read only; nothing is bound or changed.
		struct TidesProbe
		{
			std::string   last;
			std::uint32_t frames{ 0 };
		};
		TidesProbe g_tides;       // at the post-opaque point (our draw)
		TidesProbe g_tidesFrame;  // at the per-frame hook (after the previous frame, water included)

		std::string DescribeSRV(ID3D11ShaderResourceView* v)
		{
			if (!v) {
				return "-";
			}
			ID3D11Resource* res = nullptr;
			v->GetResource(&res);
			std::string out = "srv";
			if (res) {
				ID3D11Texture2D* t = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
					D3D11_TEXTURE2D_DESC d{};
					t->GetDesc(&d);
					out = std::format("{}x{} fmt {} mips {}", d.Width, d.Height, static_cast<int>(d.Format), d.MipLevels);
					t->Release();
				}
				res->Release();
			}
			return out;
		}

		std::string DescribeCB(ID3D11Buffer* b)
		{
			if (!b) {
				return "-";
			}
			D3D11_BUFFER_DESC d{};
			b->GetDesc(&d);
			return std::format("{} B", d.ByteWidth);
		}

		// Borrowed Surface Tides field: the first stage (PS, DS, VS) that has both t96 and b11 bound, checked to
		// look like SurfaceField.hlsli (square 2D texture, constant buffer of at least 80 bytes). Looked for at the
		// per-frame hook and at our draw; kept (one reference each) while found, dropped after 60 frames in which
		// neither look found it.
		struct BorrowedTides
		{
			ID3D11ShaderResourceView* srv{ nullptr };
			ID3D11Buffer*             cb{ nullptr };
			int                       misses{ 0 };
			UINT                      size{ 0 };
			const char*               stage{ "" };
			bool                      logged{ false };
			bool                      foundThisFrame{ false };
			int                       dropLogs{ 0 };  // "no longer bound" lines written (at most 3)
		};
		BorrowedTides g_borrow;

		// The borrowed field's curvature, one texel per cell, redrawn each frame the field is in use.
		ID3D11PixelShader*        g_curvPS{ nullptr };
		ID3D11Texture2D*          g_curvTex{ nullptr };
		ID3D11RenderTargetView*   g_curvRTV{ nullptr };
		ID3D11ShaderResourceView* g_curvSRV{ nullptr };
		UINT                      g_curvSize{ 0 };
		ID3D11Texture2D*          g_curvStage{ nullptr };  // a CPU copy of the curvature for the log (every 5 s)
		bool                      g_curvStagePending{ false };
		int                       g_curvLogs{ 0 };
		std::chrono::steady_clock::time_point g_curvLogAt{};

		UINT g_curvFailedSize = 0;

		bool EnsureCurvature(ID3D11DeviceContext* ctx, UINT size)
		{
			if (g_curvTex && g_curvSize == size) {
				return true;
			}
			if (g_curvFailedSize == size) {
				return false;  // failed at this size: not every frame again
			}
			Release(g_curvSRV);
			Release(g_curvRTV);
			Release(g_curvTex);
			Release(g_curvStage);
			g_curvStagePending = false;
			g_curvSize = 0;
			ID3D11Device* dev = nullptr;
			ctx->GetDevice(&dev);
			if (!dev) {
				return false;
			}
			bool ok = g_curvPS || SUCCEEDED(dev->CreatePixelShader(g_CausticCurvPS, sizeof(g_CausticCurvPS), nullptr, &g_curvPS));
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = size;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_curvTex)) && SUCCEEDED(dev->CreateRenderTargetView(g_curvTex, nullptr, &g_curvRTV)) &&
			     SUCCEEDED(dev->CreateShaderResourceView(g_curvTex, nullptr, &g_curvSRV));
			dev->Release();
			if (!ok) {
				Release(g_curvSRV);
				Release(g_curvRTV);
				Release(g_curvTex);
				logger::warn("draw pass: curvature map ({}x{}) could not be created - the tides field is not used", size, size);
				g_curvFailedSize = size;
				return false;
			}
			g_curvSize = size;
			return true;
		}

		// The borrowed field's ripples in the log (the first three times, then every 5 s with debug): the RMS of the
		// larger principal curvature, and the distance a reflection off such a ripple focuses at (1 / 2 kappa). Water
		// that focuses kilometres away is near flat: its caustics are faint whatever the shader does.
		void LogFieldStats(ID3D11DeviceContext* ctx)
		{
			if (!g_curvTex || (g_curvLogs >= 3 && !Settings::Get().debug)) {
				return;
			}
			if (g_curvStagePending) {
				D3D11_MAPPED_SUBRESOURCE m{};
				if (FAILED(ctx->Map(g_curvStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
					return;  // the copy is not done yet: next frame
				}
				double ss = 0.0;
				float  mx = 0.0f;
				UINT   n = 0;
				for (UINT y = 0; y < g_curvSize; ++y) {
					const auto* row = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch);
					for (UINT x = 0; x < g_curvSize; ++x) {
						const float hxx = row[4 * x], hyy = row[4 * x + 1], hxy = row[4 * x + 2];
						const float k = std::fabs(0.5f * (hxx + hyy)) + std::sqrt(0.25f * (hxx - hyy) * (hxx - hyy) + hxy * hxy);
						if (std::isfinite(k) && k > 0.0f) {  // cells the field covers (outside it the height is flat: exactly 0)
							ss += static_cast<double>(k) * k;
							mx = std::max(mx, k);
							++n;
						}
					}
				}
				ctx->Unmap(g_curvStage, 0);
				g_curvStagePending = false;
				++g_curvLogs;
				const double kr = n ? std::sqrt(ss / n) : 0.0;
				const double f = kr > 0.0 ? 0.5 / kr : 1e9;
				if (n == 0) {
					logger::info("surface tides field ripples: no curved cell yet (the field is empty or flat)");
				} else {
					logger::info("surface tides field ripples: RMS curvature {:.2e} per unit over {} of {} cells (max {:.2e}) -> a reflection focuses about {:.0f} units ({:.1f} m) from the water{}",
						kr, n, g_curvSize * g_curvSize, mx, f, f / 70.0, f > 3000.0 ? " - nearly flat water: its caustics are faint" : "");
				}
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (g_curvLogAt == std::chrono::steady_clock::time_point{}) {
				g_curvLogAt = now + std::chrono::seconds(2);  // the first look 2 s in: the field may still be empty when first bound
				return;
			}
			if (now < g_curvLogAt) {
				return;
			}
			g_curvLogAt = now + std::chrono::seconds(5);
			if (!g_curvStage) {
				D3D11_TEXTURE2D_DESC td{};
				g_curvTex->GetDesc(&td);
				td.Usage = D3D11_USAGE_STAGING;
				td.BindFlags = 0;
				td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ID3D11Device* dev = nullptr;
				g_curvTex->GetDevice(&dev);
				const bool ok = dev && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_curvStage));
				Release(dev);
				if (!ok) {
					g_curvLogs = 3;  // no copy: no log
					return;
				}
			}
			ctx->CopyResource(g_curvStage, g_curvTex);
			g_curvStagePending = true;
		}

		// Only Surface Tides binds its field at t96 / b11: without its DLL in the process, whatever sits there is another
		// mod's and is never borrowed.
		bool SurfaceTidesLoaded()
		{
			static const bool loaded = GetModuleHandleW(L"SurfaceTides.dll") != nullptr;
			return loaded;
		}

		// Surface Tides binds its field (PS t96, b11) only around its water draws, and our pass runs before the water:
		// at our points the slots are empty. So the immediate context's PSSetShaderResources / PSSetConstantBuffers
		// are watched (the call goes on unchanged; nothing is replaced or blocked) and the last field Surface Tides
		// bound is kept. It rewrites that same texture every frame, so one reference reads the live surface.
		struct TidesWatch
		{
			ID3D11DeviceContext*      immediate{ nullptr };
			ID3D11ShaderResourceView* srv{ nullptr };  // AddRef'd
			ID3D11Buffer*             cb{ nullptr };   // AddRef'd (only a buffer >= 80 bytes: another mod's 16-byte b11 is ignored)
			DWORD                     thread{ 0 };     // the thread our frame runs on (set at each frame start): other threads' binds are not watched
			std::uint64_t             frame{ 0 };      // advanced at each frame start
			std::uint64_t             srvSeen{ 0 }, cbSeen{ 0 };
			bool                      installed{ false };
			// every t96 / b11 bind that reached the watch, and why one was not taken (the log, while nothing is borrowed)
			std::uint64_t             t96Calls{ 0 }, t96OtherCtx{ 0 }, t96OtherThread{ 0 }, t96Null{ 0 }, b11Calls{ 0 }, b11WrongSize{ 0 };
		};
		TidesWatch g_watch;

		using SetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
		using SetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
		SetSrvFn g_origSRV[3] = {};  // PS, VS, DS
		SetCbFn  g_origCB[3] = {};

		void NoteFieldSrv(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
		{
			if (v && start <= 96 && start + n > 96) {
				++g_watch.t96Calls;
				g_watch.t96OtherCtx += c != g_watch.immediate ? 1 : 0;
				g_watch.t96OtherThread += (c == g_watch.immediate && GetCurrentThreadId() != g_watch.thread) ? 1 : 0;
				g_watch.t96Null += v[96 - start] ? 0 : 1;
			}
			if (c != g_watch.immediate || !v || start > 96 || start + n <= 96 || GetCurrentThreadId() != g_watch.thread) {
				return;
			}
			ID3D11ShaderResourceView* s = v[96 - start];
			if (!s) {
				return;
			}
			if (s != g_watch.srv) {
				s->AddRef();
				if (g_watch.srv) {
					g_watch.srv->Release();
				}
				g_watch.srv = s;
			}
			g_watch.srvSeen = g_watch.frame;
		}

		void NoteFieldCb(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* v)
		{
			if (v && start <= 11 && start + n > 11) {
				++g_watch.b11Calls;
			}
			if (c != g_watch.immediate || !v || start > 11 || start + n <= 11 || GetCurrentThreadId() != g_watch.thread) {
				return;
			}
			ID3D11Buffer* b = v[11 - start];
			if (!b) {
				return;
			}
			if (b != g_watch.cb) {
				D3D11_BUFFER_DESC bd{};
				b->GetDesc(&bd);
				if (bd.ByteWidth < 80 || bd.ByteWidth > 256) {
					++g_watch.b11WrongSize;
					return;  // STSurface is 5 float4 (80 bytes): another mod's b11 (16 B, 3840 B) is not it
				}
				b->AddRef();
				if (g_watch.cb) {
					g_watch.cb->Release();
				}
				g_watch.cb = b;
			}
			g_watch.cbSeen = g_watch.frame;
		}

		// Surface Tides displaces the water by tessellation: the field is bound to the vertex / domain stages
		// (and the pixel stage for its normals when it uses them). Each stage's two binds are watched.
		template <int S>
		void STDMETHODCALLTYPE WatchSRV(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
		{
			g_origSRV[S](c, start, n, v);
			NoteFieldSrv(c, start, n, v);
		}

		template <int S>
		void STDMETHODCALLTYPE WatchCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* v)
		{
			g_origCB[S](c, start, n, v);
			NoteFieldCb(c, start, n, v);
		}

		// vtable slots of ID3D11DeviceContext (d3d11.h, Windows SDK 10.0.26100): PSSetShaderResources 8,
		// PSSetConstantBuffers 16, VSSetShaderResources 25, VSSetConstantBuffers 7, DSSetShaderResources 63,
		// DSSetConstantBuffers 66
		constexpr int kSrvSlot[3] = { 8, 25, 63 };
		constexpr int kCbSlot[3] = { 16, 7, 66 };

		void InstallTidesWatch(ID3D11DeviceContext* ctx)
		{
			if (g_watch.installed || !ctx || !SurfaceTidesLoaded()) {
				return;
			}
			HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
			const auto& rs = Settings::Get().renderer;
			const bool  enb = rs == "enb" || (rs == "auto" && d3d && GetProcAddress(d3d, "ENBGetSDKVersion"));
			if (enb && !Settings::Get().tidesWatchUnderEnb) {
				g_watch.installed = true;
				logger::info("surface tides: ENB detected - the bind watch stays off (tidesWatchUnderEnb in zones.json turns it on)");
				return;
			}
			g_watch.installed = true;  // one try
			g_watch.immediate = ctx;
			g_watch.thread = 0;  // set by the first frame: the watch is installed at data load, not on the render thread
			void** vt = *reinterpret_cast<void***>(ctx);
			DWORD  old = 0;
			if (!VirtualProtect(&vt[7], sizeof(void*) * 60, PAGE_EXECUTE_READWRITE, &old)) {  // slots 7..66
				logger::warn("surface tides: could not watch the context's resource binds - the field stays unused");
				return;
			}
			const SetSrvFn srvHooks[3] = { &WatchSRV<0>, &WatchSRV<1>, &WatchSRV<2> };
			const SetCbFn  cbHooks[3] = { &WatchCB<0>, &WatchCB<1>, &WatchCB<2> };
			for (int s = 0; s < 3; ++s) {
				g_origSRV[s] = reinterpret_cast<SetSrvFn>(vt[kSrvSlot[s]]);
				g_origCB[s] = reinterpret_cast<SetCbFn>(vt[kCbSlot[s]]);
				vt[kSrvSlot[s]] = reinterpret_cast<void*>(srvHooks[s]);
				vt[kCbSlot[s]] = reinterpret_cast<void*>(cbHooks[s]);
			}
			DWORD tmp = 0;
			VirtualProtect(&vt[7], sizeof(void*) * 60, old, &tmp);
			logger::info("surface tides: watching the immediate context's PS / VS / DS binds for its field (t96, b11)");
		}

		void CaptureTides(ID3D11DeviceContext* ctx, bool frameStart)
		{
			if (!ctx || !SurfaceTidesLoaded()) {
				return;
			}
			if (frameStart) {
				++g_watch.frame;
				g_watch.thread = GetCurrentThreadId();  // the render thread: Surface Tides binds its field on it
			}
			if (frameStart) {  // count a miss only for a whole frame without a find
				if (g_borrow.srv && !g_borrow.foundThisFrame && ++g_borrow.misses > 60) {
					Release(g_borrow.srv);
					Release(g_borrow.cb);
					Release(g_watch.srv);  // Surface Tides may have made a new one: do not keep the old texture alive
					Release(g_watch.cb);
					if (g_borrow.dropLogs++ < 3) {
						logger::info("surface tides field no longer bound - back to the caustic atlas");
					}
				}
				g_borrow.foundThisFrame = false;
			}
			struct Found
			{
				ID3D11ShaderResourceView* srv{ nullptr };
				ID3D11Buffer*             cb{ nullptr };
			};
			auto look = [&](const char* name, auto getSrv, auto getCb) -> bool {
				Found f;
				getSrv(&f.srv);
				getCb(&f.cb);
				bool ok = f.srv && f.cb;
				UINT size = 0;
				if (ok) {
					D3D11_BUFFER_DESC bd{};
					f.cb->GetDesc(&bd);
					ok = bd.ByteWidth >= 80;
					ID3D11Resource* res = nullptr;
					f.srv->GetResource(&res);
					ID3D11Texture2D* t = nullptr;
					if (ok && res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
						D3D11_TEXTURE2D_DESC td{};
						t->GetDesc(&td);
						D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
						f.srv->GetDesc(&vd);
						const bool floatFormat = td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
						                         td.Format == DXGI_FORMAT_R32G32_FLOAT || td.Format == DXGI_FORMAT_R16G16_FLOAT;
						ok = td.Width == td.Height && td.Width >= 16 && td.Width <= 2048 && td.ArraySize == 1 && td.SampleDesc.Count == 1 && floatFormat &&
						     (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
						size = td.Width;
						t->Release();
					} else {
						ok = false;
					}
					Release(res);
				}
				if (!ok) {
					Release(f.srv);
					Release(f.cb);
					return false;
				}
				if (g_borrow.srv != f.srv || g_borrow.cb != f.cb) {
					Release(g_borrow.srv);
					Release(g_borrow.cb);
					g_borrow.srv = f.srv;  // keeps the references Get* gave us
					g_borrow.cb = f.cb;
					g_borrow.size = size;
					g_borrow.stage = name;
					if (!g_borrow.logged) {
						g_borrow.logged = true;
						logger::info("surface tides field borrowed from the {} stage: {}x{} texture - caustic brightness from the real surface", name, size, size);
					}
				} else {
					Release(f.srv);
					Release(f.cb);
				}
				g_borrow.misses = 0;
				g_borrow.foundThisFrame = true;
				return true;
			};
			// the field Surface Tides bound for a water draw in the last two frames (as Get* would: AddRef'd copies)
			const bool recent = g_watch.srv && g_watch.cb && g_watch.frame - g_watch.srvSeen <= 2 && g_watch.frame - g_watch.cbSeen <= 2;
			if ((recent && look("a stage bound for its water draws", [&](ID3D11ShaderResourceView** v) { *v = g_watch.srv; g_watch.srv->AddRef(); },
							   [&](ID3D11Buffer** b) { *b = g_watch.cb; g_watch.cb->AddRef(); })) ||
				look("PS", [&](ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(96, 1, v); }, [&](ID3D11Buffer** b) { ctx->PSGetConstantBuffers(11, 1, b); }) ||
				look("DS", [&](ID3D11ShaderResourceView** v) { ctx->DSGetShaderResources(96, 1, v); }, [&](ID3D11Buffer** b) { ctx->DSGetConstantBuffers(11, 1, b); }) ||
				look("VS", [&](ID3D11ShaderResourceView** v) { ctx->VSGetShaderResources(96, 1, v); }, [&](ID3D11Buffer** b) { ctx->VSGetConstantBuffers(11, 1, b); })) {
				return;
			}
		}

		void ProbeTides(ID3D11DeviceContext* ctx, TidesProbe& state, const char* where)
		{
			if (!ctx || !Settings::Get().debug || ++state.frames % 120 != 1) {
				return;  // every ~2 s, with debug on
			}
			std::string line;
			auto stage = [&](const char* name, auto getSrv, auto getCb) {
				ID3D11ShaderResourceView* srv[2]{};
				ID3D11Buffer*             cb = nullptr;
				getSrv(srv);
				getCb(&cb);
				line += std::format(" {}: t96 {} | t97 {} | b11 {};", name, DescribeSRV(srv[0]), DescribeSRV(srv[1]), DescribeCB(cb));
				Release(srv[0]);
				Release(srv[1]);
				Release(cb);
			};
			stage("VS", [&](ID3D11ShaderResourceView** v) { ctx->VSGetShaderResources(96, 2, v); }, [&](ID3D11Buffer** b) { ctx->VSGetConstantBuffers(11, 1, b); });
			stage("DS", [&](ID3D11ShaderResourceView** v) { ctx->DSGetShaderResources(96, 2, v); }, [&](ID3D11Buffer** b) { ctx->DSGetConstantBuffers(11, 1, b); });
			stage("PS", [&](ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(96, 2, v); }, [&](ID3D11Buffer** b) { ctx->PSGetConstantBuffers(11, 1, b); });
			if (line != state.last) {
				state.last = line;
				logger::info("surface tides probe at {}:{}", where, line);
			}
		}

		void TimerCollect(ID3D11DeviceContext* ctx)
		{
			for (int i = 0; i < GpuTimer::kRing; ++i) {
				if (!g_timer.pending[i]) {
					continue;
				}
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				if (ctx->GetData(g_timer.disjoint[i], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
					continue;  // not ready yet
				}
				UINT64 t0 = 0, t1 = 0;
				if (ctx->GetData(g_timer.begin[i], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
					ctx->GetData(g_timer.end[i], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
					continue;
				}
				g_timer.pending[i] = false;
				if (!dj.Disjoint && dj.Frequency > 0 && t1 >= t0) {
					const double ms = 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(dj.Frequency);
					g_timer.msSum += ms;
					g_timer.msMax = std::max(g_timer.msMax, ms);
					++g_timer.samples;
				}
			}
		}

		// Returns the ring slot in use, or -1 (no timing this frame).
		int TimerBegin(ID3D11DeviceContext* ctx)
		{
			if (g_timer.failed) {
				return -1;
			}
			if (!g_timer.created) {
				g_timer.created = true;
				ID3D11Device* dev = nullptr;
				ctx->GetDevice(&dev);
				if (!dev) {
					g_timer.failed = true;
					return -1;
				}
				D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
				D3D11_QUERY_DESC qt{ D3D11_QUERY_TIMESTAMP, 0 };
				for (int i = 0; i < GpuTimer::kRing && !g_timer.failed; ++i) {
					g_timer.failed = FAILED(dev->CreateQuery(&qd, &g_timer.disjoint[i])) || FAILED(dev->CreateQuery(&qt, &g_timer.begin[i])) ||
					                 FAILED(dev->CreateQuery(&qt, &g_timer.end[i]));
				}
				dev->Release();
				if (g_timer.failed) {
					for (int i = 0; i < GpuTimer::kRing; ++i) {
						Release(g_timer.disjoint[i]);
						Release(g_timer.begin[i]);
						Release(g_timer.end[i]);
					}
					logger::warn("draw pass: GPU timer queries unavailable - no GPU time in the log");
					return -1;
				}
			}
			TimerCollect(ctx);
			const int slot = g_timer.next;
			if (g_timer.pending[slot]) {
				return -1;  // the GPU is more than 4 frames behind: skip timing this frame
			}
			ctx->Begin(g_timer.disjoint[slot]);
			ctx->End(g_timer.begin[slot]);
			return slot;
		}

		void TimerEnd(ID3D11DeviceContext* ctx, int slot)
		{
			if (slot < 0) {
				return;
			}
			ctx->End(g_timer.end[slot]);
			ctx->End(g_timer.disjoint[slot]);
			g_timer.pending[slot] = true;
			g_timer.next = (slot + 1) % GpuTimer::kRing;
		}

		std::uint32_t EngineFrame()
		{
			auto* st = RE::BSGraphics::State::GetSingleton();
			return st ? st->frameCount : 0;
		}

		ID3D11Device* Device()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder) : nullptr;
		}

		ID3D11DeviceContext* Context()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11DeviceContext*>(r->GetRuntimeData().context) : nullptr;
		}

		bool IsFloatFormat(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R32G32B32_FLOAT:
			case DXGI_FORMAT_R16G16_FLOAT:
			case DXGI_FORMAT_R32G32_FLOAT:
				return true;
			default:
				return false;
			}
		}

		// BC7 DDS with a DX10 header, all mips, into an immutable texture. false + a log line on any problem.
		bool LoadAtlasDDS(ID3D11Device* dev, const char* path, ID3D11ShaderResourceView** srv)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f) {
				logger::warn("draw pass: {} not readable", path);
				return false;
			}
			f.seekg(0, std::ios::end);
			const auto length = static_cast<std::streamoff>(f.tellg());
			if (length <= 0 || length > (64ll << 20)) {
				logger::warn("draw pass: {} has an unexpected size ({} bytes)", path, static_cast<long long>(length));
				return false;
			}
			std::vector<char> b(static_cast<std::size_t>(length));
			f.seekg(0, std::ios::beg);
			f.read(b.data(), length);  // one read (the file is 5.5 MB)
			auto u32 = [&](std::size_t o) {
				std::uint32_t v = 0;
				std::memcpy(&v, b.data() + o, 4);
				return v;
			};
			if (b.size() < 148 || std::memcmp(b.data(), "DDS ", 4) != 0 || std::memcmp(b.data() + 84, "DX10", 4) != 0) {
				logger::warn("draw pass: {} is not a DX10 DDS", path);
				return false;
			}
			const int  h = static_cast<int>(u32(12)), w = static_cast<int>(u32(16)), mips = std::clamp(static_cast<int>(u32(28)), 1, 14);
			if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
				logger::warn("draw pass: {} has an unexpected size {}x{}", path, w, h);
				return false;
			}
			const auto fmt = static_cast<DXGI_FORMAT>(u32(128));
			if (fmt != DXGI_FORMAT_BC7_UNORM && fmt != DXGI_FORMAT_BC7_UNORM_SRGB) {
				logger::warn("draw pass: {} is format {}, not BC7", path, static_cast<int>(fmt));
				return false;
			}
			std::vector<D3D11_SUBRESOURCE_DATA> sub(static_cast<std::size_t>(mips));
			std::size_t                         off = 148;
			for (int m = 0; m < mips; ++m) {
				const int         mw = std::max(1, w >> m), mh = std::max(1, h >> m);
				const UINT        pitch = static_cast<UINT>(std::max(1, (mw + 3) / 4) * 16);
				const std::size_t size = static_cast<std::size_t>(pitch) * static_cast<std::size_t>(std::max(1, (mh + 3) / 4));
				if (off + size > b.size()) {
					logger::warn("draw pass: {} is truncated at mip {}", path, m);
					return false;
				}
				sub[static_cast<std::size_t>(m)] = { b.data() + off, pitch, 0 };
				off += size;
			}
			D3D11_TEXTURE2D_DESC td{};
			td.Width = static_cast<UINT>(w);
			td.Height = static_cast<UINT>(h);
			td.MipLevels = static_cast<UINT>(mips);
			td.ArraySize = 1;
			td.Format = fmt;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			ID3D11Texture2D* tex = nullptr;
			HRESULT          hr = dev->CreateTexture2D(&td, sub.data(), &tex);
			if (SUCCEEDED(hr)) {
				hr = dev->CreateShaderResourceView(tex, nullptr, srv);
				tex->Release();
			}
			if (FAILED(hr)) {
				logger::warn("draw pass: {} texture creation failed (0x{:08X})", path, static_cast<std::uint32_t>(hr));
				return false;
			}
			logger::info("draw pass: atlas {} read by the plugin: {}x{}, {} mips", path, w, h, mips);
			return true;
		}

		ID3D11ShaderResourceView* CurrentAtlasSRV()
		{
			auto* src = netimmerse_cast<RE::NiSourceTexture*>(g_atlas.get());
			return src && src->rendererTexture ? src->rendererTexture->resourceView : nullptr;
		}

		math::Mat4 ToMat4(const DirectX::SimpleMath::Matrix& m)
		{
			static_assert(sizeof(DirectX::SimpleMath::Matrix) == sizeof(float) * 16);
			math::Mat4 out;
			std::memcpy(out.m, &m, sizeof(out.m));
			return out;
		}

		// Everything Render changes, saved on entry and put back on exit.
		//
		// Binding the main colour target as our render target makes the runtime force-unbind
		// every SRV of that texture on every stage, so the SRV slots of all six stages are saved
		// (16 each; the engine does not use compute or tessellation at this point, but saving them
		// costs nothing). Restore order matters:
		//   1. unbind our PS t0/t1 (the depth SRV must be gone before the engine's DSV returns),
		//   2. restore the output merger (our RTV of the main target is replaced),
		//   3. restore all SRV slots (a saved SRV of the main target is no longer in conflict),
		//   4. the rest.
		// Not saved: OM UAVs (not used by the engine here; OMSetRenderTargets is not given any),
		// shader class instances (not used by the engine), vertex/index buffers (not touched).
		class StateSave
		{
		public:
			explicit StateSave(ID3D11DeviceContext* c) :
				_c(c)
			{
				_c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, _rtv, &_dsv);
				_c->OMGetBlendState(&_blend, _blendFactor, &_sampleMask);
				_c->OMGetDepthStencilState(&_ds, &_stencilRef);
				_c->RSGetState(&_rs);
				_c->RSGetViewports(&_vpCount, _vp);
				_c->RSGetScissorRects(&_scCount, _sc);
				_c->IAGetPrimitiveTopology(&_topo);
				_c->IAGetInputLayout(&_layout);
				_c->VSGetShader(&_vs, nullptr, nullptr);
				_c->PSGetShader(&_ps, nullptr, nullptr);
				_c->GSGetShader(&_gs, nullptr, nullptr);
				_c->HSGetShader(&_hs, nullptr, nullptr);
				_c->DSGetShader(&_dsh, nullptr, nullptr);
				_c->PSGetConstantBuffers(0, 2, _psCB);  // b0 ours, b1 the borrowed Surface Tides patch
				_c->PSGetSamplers(0, 1, &_psSampler);
				_c->VSGetShaderResources(0, kSrvSlots, _srv[kVS]);
				_c->HSGetShaderResources(0, kSrvSlots, _srv[kHS]);
				_c->DSGetShaderResources(0, kSrvSlots, _srv[kDS]);
				_c->GSGetShaderResources(0, kSrvSlots, _srv[kGS]);
				_c->PSGetShaderResources(0, kSrvSlots, _srv[kPS]);
				_c->CSGetShaderResources(0, kSrvSlots, _srv[kCS]);
			}

			~StateSave()
			{
				ID3D11ShaderResourceView* none[10]{};
				_c->PSSetShaderResources(0, 10, none);  // 1. our depth, calm atlas, water mask, normal + rough atlases, tides field + curvature, reflection atlases out
				_c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, _rtv, _dsv);  // 2.
				_c->VSSetShaderResources(0, kSrvSlots, _srv[kVS]);  // 3.
				_c->HSSetShaderResources(0, kSrvSlots, _srv[kHS]);
				_c->DSSetShaderResources(0, kSrvSlots, _srv[kDS]);
				_c->GSSetShaderResources(0, kSrvSlots, _srv[kGS]);
				_c->PSSetShaderResources(0, kSrvSlots, _srv[kPS]);
				_c->CSSetShaderResources(0, kSrvSlots, _srv[kCS]);
				_c->OMSetBlendState(_blend, _blendFactor, _sampleMask);  // 4.
				_c->OMSetDepthStencilState(_ds, _stencilRef);
				_c->RSSetState(_rs);
				_c->RSSetViewports(_vpCount, _vp);
				_c->RSSetScissorRects(_scCount, _sc);
				_c->IASetPrimitiveTopology(_topo);
				_c->IASetInputLayout(_layout);
				_c->VSSetShader(_vs, nullptr, 0);
				_c->PSSetShader(_ps, nullptr, 0);
				_c->GSSetShader(_gs, nullptr, 0);
				_c->HSSetShader(_hs, nullptr, 0);
				_c->DSSetShader(_dsh, nullptr, 0);
				_c->PSSetConstantBuffers(0, 2, _psCB);
				_c->PSSetSamplers(0, 1, &_psSampler);

				for (auto*& v : _rtv) {
					Release(v);
				}
				for (auto& stage : _srv) {
					for (auto*& v : stage) {
						Release(v);
					}
				}
				Release(_dsv);
				Release(_blend);
				Release(_ds);
				Release(_rs);
				Release(_layout);
				Release(_vs);
				Release(_ps);
				Release(_gs);
				Release(_hs);
				Release(_dsh);
				Release(_psCB[0]);
				Release(_psCB[1]);
				Release(_psSampler);
			}

			StateSave(const StateSave&) = delete;
			StateSave& operator=(const StateSave&) = delete;

		private:
			static constexpr UINT kSrvSlots = 16;
			enum Stage
			{
				kVS,
				kHS,
				kDS,
				kGS,
				kPS,
				kCS,
				kStages
			};

			ID3D11DeviceContext*      _c;
			ID3D11RenderTargetView*   _rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   _dsv{ nullptr };
			ID3D11BlendState*         _blend{ nullptr };
			FLOAT                     _blendFactor[4]{};
			UINT                      _sampleMask{ 0xFFFFFFFF };
			ID3D11DepthStencilState*  _ds{ nullptr };
			UINT                      _stencilRef{ 0 };
			ID3D11RasterizerState*    _rs{ nullptr };
			UINT                      _vpCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_VIEWPORT            _vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      _scCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_RECT                _sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			D3D11_PRIMITIVE_TOPOLOGY  _topo{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
			ID3D11InputLayout*        _layout{ nullptr };
			ID3D11VertexShader*       _vs{ nullptr };
			ID3D11PixelShader*        _ps{ nullptr };
			ID3D11GeometryShader*     _gs{ nullptr };
			ID3D11HullShader*         _hs{ nullptr };
			ID3D11DomainShader*       _dsh{ nullptr };
			ID3D11Buffer*             _psCB[2]{};
			ID3D11SamplerState*       _psSampler{ nullptr };
			ID3D11ShaderResourceView* _srv[kStages][kSrvSlots]{};
		};

		// Depth to read: the main depth target's own SRV, else the engine's copies.
		ID3D11ShaderResourceView* FindDepthSRV(RE::BSGraphics::Renderer* r, const char*& source)
		{
			auto& ds = r->GetDepthStencilData().depthStencils;
			struct Candidate
			{
				RE::RENDER_TARGET_DEPTHSTENCIL id;
				const char*                    name;
			};
			static constexpr Candidate kOrder[] = {
				{ RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN, "kMAIN" },
				{ RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY, "kMAIN_COPY" },
				{ RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY, "kPOST_ZPREPASS_COPY" },
			};
			for (const auto& c : kOrder) {
				if (auto* srv = ds[c.id].depthSRV) {
					source = c.name;
					return srv;
				}
			}
			source = "none";
			return nullptr;
		}

		void FillDiagnostics(ID3D11DeviceContext* ctx, const RE::BSGraphics::RenderTargetData& main, ID3D11ShaderResourceView* depth,
			const char* depthSource, const D3D11_VIEWPORT& vp, const RE::BSGraphics::ViewData& view, const math::Mat4& ourVP, bool invertible)
		{
			auto& d = g_diag;
			d.ready = true;
			if (main.texture) {
				D3D11_TEXTURE2D_DESC td{};
				main.texture->GetDesc(&td);
				d.mainWidth = td.Width;
				d.mainHeight = td.Height;
			}
			D3D11_RENDER_TARGET_VIEW_DESC rd{};
			main.RTV->GetDesc(&rd);
			d.mainRtvFormat = rd.Format;

			ID3D11RenderTargetView* bound = nullptr;
			ID3D11DepthStencilView* boundDsv = nullptr;
			ctx->OMGetRenderTargets(1, &bound, &boundDsv);
			d.boundIsMain = bound == main.RTV;
			d.boundDsv = boundDsv != nullptr;
			Release(bound);
			Release(boundDsv);

			d.depthSource = depthSource;
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			depth->GetDesc(&sd);
			d.depthSrvFormat = sd.Format;
			d.depthDim = sd.ViewDimension;
			ID3D11Resource* res = nullptr;
			depth->GetResource(&res);
			ID3D11Texture2D* tex = nullptr;
			if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
				D3D11_TEXTURE2D_DESC td{};
				tex->GetDesc(&td);
				d.depthWidth = td.Width;
				d.depthHeight = td.Height;
				d.depthTexFormat = td.Format;
			}
			Release(tex);
			Release(res);

			d.viewport = vp;
			const math::Mat4 engine = ToMat4(view.viewProjMat);
			float            diff = 0.0f;
			for (int i = 0; i < 16; ++i) {
				diff = std::max(diff, std::fabs(engine.m[i] - ourVP.m[i]));
			}
			d.vpDiff = diff;
			d.invertible = invertible;
		}

		bool Fail(const char* what, HRESULT hr)
		{
			logger::error("draw pass: {} failed (0x{:08X}) - caustics are off", what, static_cast<std::uint32_t>(hr));
			g_createFailed = true;
			return false;
		}
	}

	bool EnsureCreated()
	{
		if (g_created) {
			return true;
		}
		if (g_createFailed) {
			return false;
		}
		auto* device = Device();
		if (!device) {
			return false;  // renderer not up yet
		}
		InstallTidesWatch(Context());

		HRESULT hr = device->CreateVertexShader(g_CausticPassVS, sizeof(g_CausticPassVS), nullptr, &g_vs);
		if (FAILED(hr)) {
			return Fail("CreateVertexShader", hr);
		}
		hr = device->CreatePixelShader(g_CausticPassPS, sizeof(g_CausticPassPS), nullptr, &g_ps);
		if (FAILED(hr)) {
			return Fail("CreatePixelShader", hr);
		}

		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = static_cast<UINT>(sizeof(math::PassConstants));
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&cb, nullptr, &g_cb);
		if (FAILED(hr)) {
			return Fail("CreateBuffer (constants)", hr);
		}

		D3D11_BLEND_DESC bd{};
		auto&            rt = bd.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ZERO;  // result = dest * src (src = 1 + caustic)
		rt.DestBlend = D3D11_BLEND_SRC_COLOR;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		hr = device->CreateBlendState(&bd, &g_blend);
		if (FAILED(hr)) {
			return Fail("CreateBlendState", hr);
		}

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		rd.ScissorEnable = TRUE;
		hr = device->CreateRasterizerState(&rd, &g_raster);
		if (FAILED(hr)) {
			return Fail("CreateRasterizerState", hr);
		}

		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		dd.StencilEnable = FALSE;
		hr = device->CreateDepthStencilState(&dd, &g_depthOff);
		if (FAILED(hr)) {
			return Fail("CreateDepthStencilState", hr);
		}

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		hr = device->CreateSamplerState(&sd, &g_sampler);
		if (FAILED(hr)) {
			return Fail("CreateSamplerState", hr);
		}

		g_created = true;
		logger::info("draw pass: shaders created (VS {} B, PS {} B, compiled at build time), constants {} B, multiply blend, "
					 "no depth test, scissored fullscreen triangle",
			sizeof(g_CausticPassVS), sizeof(g_CausticPassPS), sizeof(math::PassConstants));
		return true;
	}

	void EnsureTextures()
	{
		if (!g_ownTried) {
			if (auto* dev = Device()) {
				g_ownTried = true;
				for (std::size_t i = 0; i < g_own.size(); ++i) {
					LoadAtlasDDS(dev, kAtlasFiles[i], &g_own[i]);
				}
				if (!g_own[3] || !g_own[4] || !g_own[5]) {
					logger::warn("draw pass: the walls' reflection atlases (refl_calm / refl_normal / refl_rough.dds) did not load - walls get the mean light without a pattern");
				}
			}
		}
		if (g_own[0]) {
			return;  // the engine's copy is not needed
		}
		if (g_atlasSRV) {
			// In game the atlas first arrived as 512x512 (4 mips) instead of 2048 (6): log whether it ever
			// changes, checked every ~5 s (50 task ticks).
			static int  s_ticks = 0;
			static UINT s_w = 0, s_h = 0, s_mips = 0;
			if (++s_ticks % 50 != 1) {
				return;
			}
			ID3D11ShaderResourceView* view = CurrentAtlasSRV();
			ID3D11Resource*           res = nullptr;
			if (view) {
				view->GetResource(&res);
			}
			if (res) {
				ID3D11Texture2D* tex2D = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2D))) && tex2D) {
					D3D11_TEXTURE2D_DESC td{};
					tex2D->GetDesc(&td);
					tex2D->Release();
					if (td.Width != s_w || td.Height != s_h || td.MipLevels != s_mips) {
						logger::info("draw pass: atlas now D3D {}x{} mips {} (was {}x{} mips {})", td.Width, td.Height, td.MipLevels, s_w, s_h,
							s_mips);
						s_w = td.Width;
						s_h = td.Height;
						s_mips = td.MipLevels;
					}
				}
				res->Release();
			}
			return;
		}
		if (g_textureGaveUp) {
			return;
		}
		++g_textureAttempts;
		if (!g_atlas) {
			RE::NiPointer<RE::NiTexture> tex;
			RE::BSShaderManager::GetTexture(kCalmAtlasPath, true, tex, false);
			g_atlas = tex;
		}
		auto* src = netimmerse_cast<RE::NiSourceTexture*>(g_atlas.get());
		if (src && src->rendererTexture && src->rendererTexture->resourceView) {
			// The engine's own width/height fields read 0x0 in game, so ask D3D what the view really holds:
			// a tiny texture is the loader's placeholder. The atlas layout is in UV, so a downscaled copy
			// (the game hands out 512x512) still samples correctly, only softer.
			ID3D11ShaderResourceView* view = src->rendererTexture->resourceView;
			D3D11_TEXTURE2D_DESC      td{};
			bool                      is2D = false;
			ID3D11Resource*           res = nullptr;
			view->GetResource(&res);
			if (res) {
				ID3D11Texture2D* tex2D = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2D))) && tex2D) {
					tex2D->GetDesc(&td);
					is2D = true;
					tex2D->Release();
				}
				res->Release();
			}
			if (is2D && td.Width >= 256 && td.Height >= 256) {
				g_atlasSRV = view;
				logger::info("draw pass: atlas {} found (D3D: {}x{} mips {} format {}, try {})", kCalmAtlasPath, td.Width, td.Height,
					td.MipLevels, static_cast<int>(td.Format), g_textureAttempts);
				return;
			}
			if (g_textureAttempts == 1 || g_textureAttempts % 50 == 0) {
				logger::warn("draw pass: atlas {} view is not the atlas yet ({}) - try {}", kCalmAtlasPath,
					is2D ? std::format("D3D {}x{} format {}", td.Width, td.Height, static_cast<int>(td.Format)) : std::string("not a 2D texture"),
					g_textureAttempts);
			}
		}
		if (g_textureAttempts >= kMaxTextureAttempts) {
			g_textureGaveUp = true;
			logger::error("draw pass: atlas {} NOT available after {} tries (texture {}, source {}, renderer texture {}) - only "
						  "debugTint can draw",
				kCalmAtlasPath, g_textureAttempts, g_atlas ? "yes" : "no", src ? "yes" : "no", src && src->rendererTexture ? "yes" : "no");
		}
	}

	void Stage(const std::array<ZoneGpuInput, kMaxDrawZones>& zones, const std::array<bool, kMaxDrawZones>& used)
	{
		for (std::size_t k = 0; k < kMaxDrawZones; ++k) {
			const float clock = g_clock[k];
			g_zones[k] = zones[k];
			g_zones[k].frameClock = clock;  // the hook owns the clock between stagings
		}
		g_used = used;
	}

	void ClearStaging()
	{
		g_zones = {};
		g_used = {};
		g_clock = {};
	}

	void SetFrameClock(std::size_t slot, float clock)
	{
		if (slot < kMaxDrawZones) {
			g_clock[slot] = clock;
		}
	}

	void BeginFrame()
	{
		ProbeTides(Context(), g_tidesFrame, "the per-frame hook");
		CaptureTides(Context(), true);
		++g_second.frames;
		g_drewThisFrame = false;
		g_blockedCalls = 0;
		if (!g_statsReady) {
			if (g_beginFrames == 0) {
				g_engineFrameAtStart = EngineFrame();
				g_postCalls = 0;
			}
			if (++g_beginFrames > kStatFrames) {
				g_statCalls = g_postCalls;
				g_statEngine = EngineFrame() - g_engineFrameAtStart;
				g_statsReady = true;
			}
		}
	}

	void Render()
	{
		++g_postCalls;
		if (!g_created || !g_enabled) {
			return;
		}
		bool any = false;
		for (std::size_t k = 0; k < kMaxDrawZones; ++k) {
			any = any || g_used[k];
		}
		if (!any) {
			return;
		}
		const std::uint32_t engineFrame = EngineFrame();
		if (g_drewThisFrame && engineFrame != g_drawEngineFrame) {
			g_drewThisFrame = false;  // the engine moved to a new frame
			g_blockedCalls = 0;
		}
		if (g_drewThisFrame) {
			if (++g_blockedCalls < kUnlockAfter) {
				g_skipSeen |= kSkipSecondCall;
				return;
			}
			g_drewThisFrame = false;  // no frame signal for kUnlockAfter calls: do not stay locked
			g_blockedCalls = 0;
			++g_fallbackUnlocks;
		}

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* shadow = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto* ctx = Context();
		if (!renderer || !shadow || !ctx) {
			return;
		}

		const auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
		if (!main.RTV) {
			g_skipSeen |= kSkipNoMainRTV;
			return;
		}
		const char* depthSource = "none";
		auto*       depth = FindDepthSRV(renderer, depthSource);
		if (!depth) {
			g_skipSeen |= kSkipNoDepthSRV;
			return;
		}
		if (depth != g_depthSrvNow) {
			g_depthNamePrev = g_depthNameNow;
			g_depthNameNow = depthSource;
			g_depthSrvNow = depth;
			++g_depthChanges;
		}

		D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT           vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ctx->RSGetViewports(&vpCount, vps);
		if (vpCount == 0 || !(vps[0].Width > 0.0f) || !(vps[0].Height > 0.0f)) {
			g_skipSeen |= kSkipNoViewport;
			return;
		}
		if (main.texture) {
			D3D11_TEXTURE2D_DESC td{};
			main.texture->GetDesc(&td);
			if (vps[0].Width * 2.0f < static_cast<float>(td.Width) || vps[0].Height * 2.0f < static_cast<float>(td.Height)) {
				g_skipSeen |= kSkipSmallViewport;  // not the scene view (reflection, cube map, menu model)
				return;
			}
		}

		const auto       view = shadow->GetRuntimeData().cameraData.getEye();
		const auto       adj = shadow->GetRuntimeData().posAdjust.getEye();
		const math::Mat4 viewProj = math::Mul(ToMat4(view.viewMat), ToMat4(view.projMat));
		const auto       inv = math::Inverse(viewProj);

		if (!g_diag.ready) {
			FillDiagnostics(ctx, main, depth, depthSource, vps[0], view, viewProj, inv.has_value());
			// The engine binds its targets lazily; its shadow state says what it intends to draw into.
			const auto& sr = shadow->GetRuntimeData();
			for (int i = 0; i < 3; ++i) {
				g_diag.shadowRT[i] = static_cast<int>(sr.renderTargets[i]);
			}
			g_diag.shadowDepth = static_cast<int>(sr.depthStencil);
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC depthDesc{};
		depth->GetDesc(&depthDesc);
		if (depthDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
			g_skipSeen |= kSkipDepthNotTexture2D;  // multisampled or array depth: the shader reads Texture2D
			return;
		}
		if (!inv) {
			g_skipSeen |= kSkipSingularCamera;
			return;
		}

		ID3D11ShaderResourceView* atlas = g_own[0];
		if (!atlas) {
			atlas = g_atlasSRV ? CurrentAtlasSRV() : nullptr;
			g_atlasSRV = g_atlasSRV ? atlas : nullptr;  // follow a reloaded texture; drop a freed one
		}

		math::FrameCamera cam;
		cam.invViewProj = *inv;
		cam.viewProj = viewProj;
		cam.posAdjust = { adj.x, adj.y, adj.z };
		cam.viewport = { vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height, vps[0].MinDepth, vps[0].MaxDepth };

		std::uintptr_t boundRT0 = 0;
		if (!g_firstDraw.ready) {
			ID3D11RenderTargetView* rt0 = nullptr;
			ctx->OMGetRenderTargets(1, &rt0, nullptr);
			boundRT0 = reinterpret_cast<std::uintptr_t>(rt0);
			Release(rt0);
		}

		ProbeTides(ctx, g_tides, "the post-opaque point");
		CaptureTides(ctx, false);
		const StateSave save(ctx);

		ctx->OMSetRenderTargets(1, &main.RTV, nullptr);  // no DSV: depth is read below as a texture
		const FLOAT factor[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(g_depthOff, 0);
		ctx->RSSetState(g_raster);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->IASetInputLayout(nullptr);
		ctx->VSSetShader(g_vs, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShader(g_ps, nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, &g_cb);
		ctx->PSSetSamplers(0, 1, &g_sampler);
		ID3D11ShaderResourceView* srvs[2]{ depth, atlas };
		ctx->PSSetShaderResources(0, 2, srvs);
		ID3D11ShaderResourceView* layerSrvs[2]{ g_own[1], g_own[2] };
		ctx->PSSetShaderResources(3, 2, layerSrvs);
		ID3D11ShaderResourceView* reflSrvs[3]{ g_own[3], g_own[4], g_own[5] };
		ctx->PSSetShaderResources(7, 3, reflSrvs);
		bool tides = Settings::Get().useSurfaceTides && g_borrow.srv && g_borrow.cb && EnsureCurvature(ctx, g_borrow.size);  // CausticLight.ini bUseSurfaceTides
		if (tides) {
			ctx->PSSetShaderResources(5, 1, &g_borrow.srv);
			ctx->PSSetConstantBuffers(1, 1, &g_borrow.cb);
			// the field's curvature into its own small target, then back to the scene target
			ctx->OMSetRenderTargets(1, &g_curvRTV, nullptr);
			ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
			const D3D11_VIEWPORT cvp{ 0.0f, 0.0f, static_cast<float>(g_curvSize), static_cast<float>(g_curvSize), 0.0f, 1.0f };
			const D3D11_RECT     crect{ 0, 0, static_cast<LONG>(g_curvSize), static_cast<LONG>(g_curvSize) };
			ctx->RSSetViewports(1, &cvp);
			ctx->RSSetScissorRects(1, &crect);
			ctx->PSSetShader(g_curvPS, nullptr, 0);
			ctx->Draw(3, 0);
			ctx->OMSetRenderTargets(1, &main.RTV, nullptr);
			ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
			ctx->RSSetViewports(1, &vps[0]);
			ctx->PSSetShader(g_ps, nullptr, 0);
			ctx->PSSetShaderResources(6, 1, &g_curvSRV);
			LogFieldStats(ctx);
		}
		int timerSlot = -2;  // -2: not started (started just before the first draw)

		int                                        drawn = 0;
		std::uint64_t                              pixels = 0;
		std::array<math::PixelRect, kMaxDrawZones> rects{};
		for (std::size_t k = 0; k < kMaxDrawZones; ++k) {
			if (!g_used[k]) {
				continue;
			}
			ZoneGpuInput z = g_zones[k];
			z.frameClock = g_clock[k];
			if (!z.debugTint) {
				if (!(z.level > 0.0f) || !(z.distFade > 0.0f)) {
					continue;  // nothing to add
				}
				if (!atlas && !z.debugLight) {  // the light mask never samples the pattern
					g_skipSeen |= kSkipNoAtlas;
					continue;
				}
			}
			// the box reaches down to the deepest pool floor the pass lights (FloorPass)
			// scissor around where the water really is (the mask's water texels), not the whole mask
			const math::Rect scissorRect = z.mask && z.mask->waterTexels > 0 ? z.mask->waterBounds : z.rect;
			const auto sc = math::ScissorForZone(scissorRect, z.waterZ - math::kBottomMaxDepth, z.height + math::kBottomMaxDepth, z.height, cam.posAdjust,
				viewProj, cam.viewport);
			if (!sc) {
				continue;  // off-screen or behind the camera
			}
			ID3D11ShaderResourceView* maskSRV = MaskSRV(ctx, k, z.mask);
			if (!maskSRV && !z.debugTint) {
				continue;  // no water mask: nothing can be reflected
			}
			ctx->PSSetShaderResources(2, 1, &maskSRV);

			z.tides = tides;
			z.tidesSize = tides ? static_cast<float>(g_borrow.size) : 0.0f;
			math::PassConstants pc = math::PackPass(z, cam);
			pc.misc.w = (g_own[3] && g_own[4] && g_own[5]) ? 0.0f : 1.0f;  // no reflection atlases: a flat walls' pattern
			// where the field applies (this zone's water level, inside its patch) the shader itself narrows the spread
			// to the still surface's, drops the swimmer rings (they are in the field) and scales the reflection
			if (!g_own[1] || !g_own[2]) {  // a layer without its atlas: its weight goes to the calm one
				pc.layers.x += (g_own[1] ? 0.0f : pc.layers.y) + (g_own[2] ? 0.0f : pc.layers.z);
				pc.layers.y = g_own[1] ? pc.layers.y : 0.0f;
				pc.layers.z = g_own[2] ? pc.layers.z : 0.0f;
			}
			D3D11_MAPPED_SUBRESOURCE  mapped{};
			if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
				g_skipSeen |= kSkipMapFailed;
				break;
			}
			std::memcpy(mapped.pData, &pc, sizeof(pc));
			ctx->Unmap(g_cb, 0);

			const D3D11_RECT rect{ sc->left, sc->top, sc->right, sc->bottom };
			ctx->RSSetScissorRects(1, &rect);
			if (timerSlot == -2) {
				timerSlot = TimerBegin(ctx);
			}
			ctx->Draw(3, 0);
			rects[static_cast<std::size_t>(drawn)] = *sc;
			pixels += static_cast<std::uint64_t>(sc->right - sc->left) * static_cast<std::uint64_t>(sc->bottom - sc->top);
			++drawn;
		}
		if (timerSlot >= 0) {
			TimerEnd(ctx, timerSlot);
		}
		if (drawn > 0) {
			g_drewThisFrame = true;
			g_drawEngineFrame = engineFrame;
			++g_framesDrawn;
			++g_second.framesDrawn;
			g_second.zoneDraws += static_cast<std::uint32_t>(drawn);
			g_second.pixels += pixels;
			g_second.lastZones = drawn;
			g_second.lastPixels = pixels;
			g_second.lastViewportPixels = vps[0].Width * vps[0].Height;
			g_second.lastRects = rects;
			if (!g_firstDraw.ready) {
				auto& f = g_firstDraw;
				f.ready = true;
				f.boundRT0 = boundRT0;
				f.mainRTV = reinterpret_cast<std::uintptr_t>(main.RTV);
				f.viewport = vps[0];
				f.engineFrame = engineFrame;
				f.zones = drawn;
				f.depth = depthSource;
				if (main.texture) {
					D3D11_TEXTURE2D_DESC td{};
					main.texture->GetDesc(&td);
					f.width = td.Width;
					f.height = td.Height;
				}
			}
		}
		// `save` restores everything here
	}

	void ReportRenderInfo()
	{
		const auto& d = g_diag;
		if (d.ready && !d.logged) {
			g_diag.logged = true;
			logger::info("draw pass: main colour target {}x{} RTV format {} ({})", d.mainWidth, d.mainHeight, static_cast<int>(d.mainRtvFormat),
				IsFloatFormat(d.mainRtvFormat) ? "float: 1 + caustic can brighten" :
				                                 "NOT FLOAT - the blend clamps our output to 1, so the multiply cannot brighten anything");
			logger::info("draw pass: at the post-opaque call, render target 0 {} the main colour target{}; engine shadow state targets "
						 "{} / {} / {}, depth {} (kMAIN = {})",
				d.boundIsMain ? "IS" : "is NOT", d.boundIsMain ? "" : " (another or no target is bound on the device here)", d.shadowRT[0],
				d.shadowRT[1], d.shadowRT[2], d.shadowDepth, static_cast<int>(RE::RENDER_TARGET::kMAIN));
			logger::info("draw pass: depth from {}: {}x{} texture format {}, SRV format {}, dimension {} ({}); depth-stencil bound at the call: {}",
				d.depthSource, d.depthWidth, d.depthHeight, static_cast<int>(d.depthTexFormat), static_cast<int>(d.depthSrvFormat),
				static_cast<int>(d.depthDim), d.depthDim == D3D11_SRV_DIMENSION_TEXTURE2D ? "Texture2D, usable" : "NOT Texture2D - pass is off",
				d.boundDsv ? "yes" : "no");
			logger::info("draw pass: viewport ({:.0f}, {:.0f}) {:.0f}x{:.0f} depth {:.2f}..{:.2f}; |viewMat x projMat - viewProjMat| max {:.2e}; "
						 "camera matrix {}",
				d.viewport.TopLeftX, d.viewport.TopLeftY, d.viewport.Width, d.viewport.Height, d.viewport.MinDepth, d.viewport.MaxDepth,
				d.vpDiff, d.invertible ? "invertible" : "NOT invertible");
		}
		if (g_firstDraw.ready && !g_firstDraw.logged) {
			const auto& f = g_firstDraw;
			g_firstDraw.logged = true;
			g_firstDrawLogged = true;
			logger::info("draw pass: first draw: {} zone(s) into main RTV 0x{:X} ({}x{}); render target 0 bound at the call was 0x{:X} ({}); "
						 "viewport ({:.0f}, {:.0f}) {:.0f}x{:.0f}; depth {}; engine frame {}",
				f.zones, f.mainRTV, f.width, f.height, f.boundRT0, f.boundRT0 == f.mainRTV ? "the main target" : "NOT the main target",
				f.viewport.TopLeftX, f.viewport.TopLeftY, f.viewport.Width, f.viewport.Height, f.depth, f.engineFrame);
		}
		if (g_depthChanges != g_depthChangesLogged) {
			logger::info("draw pass: depth source is now {} (was {}){}", g_depthNameNow, g_depthNamePrev,
				g_depthChanges - g_depthChangesLogged > 1 ? std::format(", {} changes since the last report", g_depthChanges - g_depthChangesLogged) :
				                                            std::string());
			g_depthChangesLogged = g_depthChanges;
		}
		if (g_fallbackUnlocks > 0 && !g_fallbackUnlockLogged) {
			g_fallbackUnlockLogged = true;
			logger::warn("draw pass: {} post-opaque calls in a row saw no frame signal (neither the per-frame hook nor the engine frame "
						 "counter moved); the once-per-frame lock was released so drawing continues",
				kUnlockAfter);
		}
		if (g_statsReady && !g_statsLogged) {
			g_statsLogged = true;
			logger::info("draw pass: over {} frames (per-frame hook): {} post-opaque calls ({:.2f} per frame), engine frame counter advanced {} "
						 "({:.2f} per frame)",
				kStatFrames, g_statCalls, static_cast<double>(g_statCalls) / kStatFrames, g_statEngine,
				static_cast<double>(g_statEngine) / kStatFrames);
		}
		const std::uint32_t fresh = g_skipSeen & ~g_skipLogged;
		if (fresh) {
			g_skipLogged |= fresh;
			struct Reason
			{
				std::uint32_t bit;
				const char*   text;
			};
			static constexpr Reason kReasons[] = {
				{ kSkipNoMainRTV, "no main colour target view" },
				{ kSkipNoDepthSRV, "no readable depth (kMAIN, kMAIN_COPY and kPOST_ZPREPASS_COPY have no SRV)" },
				{ kSkipNoViewport, "no viewport bound" },
				{ kSkipSmallViewport, "viewport smaller than half the main target (not the scene view; expected now and then)" },
				{ kSkipDepthNotTexture2D, "depth SRV is not a plain Texture2D" },
				{ kSkipNoAtlas, "calm atlas not loaded" },
				{ kSkipSingularCamera, "camera matrix not invertible" },
				{ kSkipMapFailed, "constant buffer Map failed" },
				{ kSkipSecondCall, "post-opaque call seen twice in one frame; drew only the first (no double brightening)" },
			};
			for (const auto& r : kReasons) {
				if (fresh & r.bit) {
					logger::warn("draw pass: skipped a frame: {}", r.text);
				}
			}
		}
	}

	void SetEnabled(bool on)
	{
		g_enabled = on;
	}

	bool Enabled()
	{
		return g_enabled;
	}

	void WatchTidesEarly()
	{
		// before Surface Tides hooks the renderer (its first frame after a load): a copy of the context's functions it
		// keeps then points at the watch, not past it
		if (Device()) {
			InstallTidesWatch(Context());
		}
	}

	void LogSecond()
	{
		// while no field is borrowed: what reached the watch (every 10 s), so one test tells why
		static int s_tidesTicks = 0;
		if (g_watch.installed && g_watch.immediate && !g_borrow.srv && ++s_tidesTicks % 10 == 0) {
			logger::info("surface tides watch: t96 binds {} (another context {}, another thread {}, empty {}), b11 binds {} ({} of the wrong size), field kept {}",
				g_watch.t96Calls, g_watch.t96OtherCtx, g_watch.t96OtherThread, g_watch.t96Null, g_watch.b11Calls, g_watch.b11WrongSize,
				g_watch.srv ? "yes" : "no");
		}
		const SecondStats s = g_second;
		g_second = {};
		if (s.framesDrawn == 0) {
			logger::info("draw pass {}: drew in 0 of {} frames this second", g_enabled ? "ON" : "OFF", s.frames);
			return;
		}
		std::string rects;
		for (int i = 0; i < s.lastZones; ++i) {
			const auto& r = s.lastRects[static_cast<std::size_t>(i)];
			rects += std::format("{}({},{})-({},{})", i ? " " : "", r.left, r.top, r.right, r.bottom);
		}
		if (g_borrow.srv) {
			ID3D11Resource* res = nullptr;
			g_borrow.srv->GetResource(&res);
			logger::info("surface tides field: srv {} resource {} ({}x{}, from {}), frames without a find {}", static_cast<const void*>(g_borrow.srv),
				static_cast<const void*>(res), g_borrow.size, g_borrow.size, g_borrow.stage, g_borrow.misses);
			Release(res);
		}
		const std::string gpu = g_timer.samples ? std::format("GPU {:.3f} ms avg, {:.3f} ms max ({} frames timed)", g_timer.msSum / g_timer.samples,
															  g_timer.msMax, g_timer.samples) :
		                                          std::string("GPU time n/a");
		g_timer.msSum = g_timer.msMax = 0.0;
		g_timer.samples = 0;
		logger::info("draw pass {}: drew in {} of {} frames this second, {} zone draws, avg {:.0f} px/frame, {}; last frame {} zone(s), {} px "
					 "({:.1f} % of the viewport) rects {}",
			g_enabled ? "ON" : "OFF", s.framesDrawn, s.frames, s.zoneDraws, static_cast<double>(s.pixels) / s.framesDrawn, gpu, s.lastZones,
			s.lastPixels, s.lastViewportPixels > 0.0f ? 100.0 * static_cast<double>(s.lastPixels) / s.lastViewportPixels : 0.0, rects);
	}

	void NoteHookInstalled(bool installed)
	{
		g_hookInstalled = installed;
	}

	bool Ready()
	{
		return g_created && g_hookInstalled;
	}

	bool AtlasReady()
	{
		return g_own[0] != nullptr || g_atlasSRV != nullptr;
	}
}
