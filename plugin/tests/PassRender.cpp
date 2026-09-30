// Offline GPU check of the real draw pass: renders a small synthetic pond scene with D3D11 (WARP, no game),
// then runs the embedded CausticPass VS/PS over it exactly as the plugin does (PackPass constants, multiply
// blend, depth read as a texture, scissor from ScissorForZone, the shipped calm atlas).
//
//   PassRender.exe <calm.dds> <out dir>
//
// Checks:
//   * light mask (CL_DebugTint 2): per-pixel against the C++ mirror CausticBoost, and per surface: the
//     undersides and the walls facing the light's mirror image are lit, upward floors and walls facing
//     away are not.
//   * the walls' reflection pattern (ReflMix: refl_*.dds by distance, variance-preserving) is GPU only: the C++ mirror
//     takes one scalar pattern value, so the GPU vs mirror checks here run without a pattern; the pattern is checked
//     against Cycles by tools/blender-caustic-stats.py / -compare.py on the dumps.
//   * caustics (CL_Strength 2.14): lit surfaces get a patterned boost within 0..+280 % (kMaxBoost; the floor +150 %, kMaxFloorBoost), the rest is unchanged.
// Writes base / mask / caustic images as .ppm into <out dir> (tools/ppm2png.cjs turns them into PNGs).

#include "CausticMath.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <optional>
#include <type_traits>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "CausticPassPS.h"
#include "CausticPassVS.h"
#include "CausticCurvPS.h"

using namespace cl::math;

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void Check(bool ok, const char* what, double got = 0.0, double want = 0.0)
	{
		++g_checks;
		if (!ok) {
			++g_failures;
			std::printf("  FAIL  %s (got %.5f, want %.5f)\n", what, got, want);
		} else {
			std::printf("  ok    %s\n", what);
		}
	}

	template <class T>
	void Release(T*& p)
	{
		if (p) {
			p->Release();
			p = nullptr;
		}
	}

	constexpr int W = 640, H = 400;

	// ---- the synthetic scene (world units, z up, like Skyrim) ------------------------------------
	// Pond: water rect (-200,-200)-(200,200) at z 0, bottom at z -60 (the water surface itself is not in
	// the depth buffer, as in game: water is drawn after the opaque pass).
	// Lantern north-east and high: (600, 600, 450). Its mirror image is at (600, 600, -450), so the
	// reflected light travels up and to the south-west.
	enum Surface : std::uint32_t
	{
		kNone = 0,
		kPondBottom,    // under the water: rejected (h < 2)
		kBridgeUnder,   // faces down, 120 above the water: lit
		kBridgeTop,     // faces up: never lit
		kWallSW_X,      // x = -230, faces +x (toward the light): lit
		kWallSW_Y,      // y = -230, faces +y: lit
		kWallNE_X,      // x = +230, faces -x (away from the light): not lit
		kBridgeSide,    // the bridge's north (faces +y, lit) and south (faces -y, not lit) sides
		kSurfaces
	};
	const char* kSurfaceNames[kSurfaces]{ "none", "pond bottom", "bridge underside", "bridge top", "south-west wall (x)",
		"south-west wall (y)", "north-east wall", "bridge sides" };

	struct Vertex
	{
		float    p[3];
		float    n[3];
		float    albedo;
		uint32_t id;
	};

	void Quad(std::vector<Vertex>& v, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& n, float albedo, Surface id)
	{
		const Vec3 q[6]{ a, b, c, a, c, d };
		for (const auto& p : q) {
			v.push_back({ { p[0], p[1], p[2] }, { n[0], n[1], n[2] }, albedo, id });
		}
	}

	std::vector<Vertex> BuildScene()
	{
		std::vector<Vertex> v;
		Quad(v, { -230, -230, -60 }, { 230, -230, -60 }, { 230, 230, -60 }, { -230, 230, -60 }, { 0, 0, 1 }, 0.35f, kPondBottom);
		// bridge: x -230..230, y -40..40, underside z 120, top z 135
		Quad(v, { -230, -40, 120 }, { 230, -40, 120 }, { 230, 40, 120 }, { -230, 40, 120 }, { 0, 0, -1 }, 0.5f, kBridgeUnder);
		Quad(v, { -230, -40, 135 }, { 230, -40, 135 }, { 230, 40, 135 }, { -230, 40, 135 }, { 0, 0, 1 }, 0.5f, kBridgeTop);
		Quad(v, { -230, 40, 120 }, { 230, 40, 120 }, { 230, 40, 135 }, { -230, 40, 135 }, { 0, 1, 0 }, 0.45f, kBridgeSide);
		Quad(v, { -230, -40, 120 }, { 230, -40, 120 }, { 230, -40, 135 }, { -230, -40, 135 }, { 0, -1, 0 }, 0.45f, kBridgeSide);
		Quad(v, { -230, -230, -60 }, { -230, 230, -60 }, { -230, 230, 450 }, { -230, -230, 450 }, { 1, 0, 0 }, 0.6f, kWallSW_X);
		Quad(v, { -230, -230, -60 }, { 230, -230, -60 }, { 230, -230, 450 }, { -230, -230, 450 }, { 0, 1, 0 }, 0.6f, kWallSW_Y);
		Quad(v, { 230, -230, -60 }, { 230, 230, -60 }, { 230, 230, 450 }, { 230, -230, 450 }, { -1, 0, 0 }, 0.6f, kWallNE_X);
		return v;
	}

	// ---- camera: rel = world - posAdjust (posAdjust = the eye, as in game) --------------------------
	struct Camera
	{
		Vec3 eye;
		Mat4 viewProj;
		Mat4 invViewProj;
	};

	Vec3 Norm(const Vec3& a)
	{
		const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
		return { a[0] / l, a[1] / l, a[2] / l };
	}
	Vec3 Cross(const Vec3& a, const Vec3& b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }

	Camera MakeCamera(const Vec3& eye, const Vec3& target)
	{
		const Vec3 f = Norm({ target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] });
		const Vec3 r = Norm(Cross(f, { 0, 0, 1 }));
		const Vec3 u = Cross(r, f);
		Mat4       view{};  // row vectors: [x y z 1] * view = [dot(p,r), dot(p,u), dot(p,f), 1] (p = rel)
		for (int i = 0; i < 3; ++i) {
			view.at(i, 0) = r[i];
			view.at(i, 1) = u[i];
			view.at(i, 2) = f[i];
		}
		view.at(3, 3) = 1.0f;
		const float n = 5.0f, fa = 5000.0f, fovY = 70.0f * 3.14159265f / 180.0f;
		const float sy = 1.0f / std::tan(fovY / 2), sx = sy * H / W;
		Mat4        proj{};
		proj.at(0, 0) = sx;
		proj.at(1, 1) = sy;
		proj.at(2, 2) = fa / (fa - n);
		proj.at(2, 3) = 1.0f;
		proj.at(3, 2) = -n * fa / (fa - n);
		Camera c;
		c.eye = eye;
		c.viewProj = Mul(view, proj);
		c.invViewProj = *Inverse(c.viewProj);
		return c;
	}

	// ---- minimal DDS (DX10 header, BC7) -------------------------------------------------------------
	bool LoadDDS(ID3D11Device* dev, const char* path, ID3D11ShaderResourceView** srv, int& w, int& h, int& mips)
	{
		std::ifstream f(path, std::ios::binary);
		std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (b.size() < 148 || std::memcmp(b.data(), "DDS ", 4) != 0) {
			return false;
		}
		auto u32 = [&](size_t o) { std::uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
		h = static_cast<int>(u32(12));
		w = static_cast<int>(u32(16));
		mips = std::max<int>(1, static_cast<int>(u32(28)));
		if (std::memcmp(b.data() + 84, "DX10", 4) != 0) {
			return false;
		}
		const auto fmt = static_cast<DXGI_FORMAT>(u32(128));
		if (fmt != DXGI_FORMAT_BC7_UNORM && fmt != DXGI_FORMAT_BC7_UNORM_SRGB) {
			return false;
		}
		std::vector<D3D11_SUBRESOURCE_DATA> sub(mips);
		size_t off = 148;
		for (int m = 0; m < mips; ++m) {
			const int    mw = std::max(1, w >> m), mh = std::max(1, h >> m);
			const UINT   pitch = static_cast<UINT>(std::max(1, (mw + 3) / 4) * 16);
			const size_t size = static_cast<size_t>(pitch) * std::max(1, (mh + 3) / 4);
			if (off + size > b.size()) {
				return false;
			}
			sub[m] = { b.data() + off, pitch, 0 };
			off += size;
		}
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = mips;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* tex = nullptr;
		if (FAILED(dev->CreateTexture2D(&td, sub.data(), &tex))) {
			return false;
		}
		const HRESULT hr = dev->CreateShaderResourceView(tex, nullptr, srv);
		tex->Release();
		return SUCCEEDED(hr);
	}

	const char* kSceneHLSL = R"(
cbuffer Scene : register(b0) { row_major float4x4 ViewProj; float4 Eye; };
struct VSIn { float3 p : POSITION; float3 n : NORMAL; float albedo : ALBEDO; uint id : SURFACE; };
struct VSOut { float4 pos : SV_Position; float3 n : NORMAL; float albedo : ALBEDO; nointerpolation uint id : SURFACE; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.p - Eye.xyz, 1), ViewProj); o.n = i.n; o.albedo = i.albedo; o.id = i.id; return o; }
struct PSOut { float4 c : SV_Target0; uint id : SV_Target1; };
PSOut PSMain(VSOut i) { PSOut o; float s = i.albedo * (0.35 + 0.25 * saturate(i.n.z + 0.5) + 0.15 * abs(i.n.x));
	o.c = float4(s, s * 0.95, s * 0.9, 1); o.id = i.id; return o; }
)";

	struct Readback
	{
		std::vector<float>         rgb;  // W*H*3
		std::vector<std::uint32_t> id;
		std::vector<float>         depth;
	};

	// Raw little-endian dumps for tools/blender-caustic-compare.py (W x H, row 0 = top).
	template <class T>
	void WriteRaw(const std::string& path, const std::vector<T>& v)
	{
		std::ofstream f(path, std::ios::binary);
		f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
	}

	void WritePPM(const std::string& path, const std::vector<float>& rgb)
	{
		std::ofstream f(path, std::ios::binary);
		f << "P6\n" << W << " " << H << "\n255\n";
		for (float v : rgb) {
			const float s = std::sqrt(std::clamp(v, 0.0f, 1.0f));  // rough gamma so the dark scene reads
			f.put(static_cast<char>(static_cast<unsigned char>(s * 255.0f + 0.5f)));
		}
	}

	struct Gpu
	{
		ID3D11Device*             dev = nullptr;
		ID3D11DeviceContext*      ctx = nullptr;
		ID3D11Texture2D*          color = nullptr;
		ID3D11RenderTargetView*   colorRTV = nullptr;
		ID3D11Texture2D*          idTex = nullptr;
		ID3D11RenderTargetView*   idRTV = nullptr;
		ID3D11Texture2D*          depth = nullptr;
		ID3D11DepthStencilView*   dsv = nullptr;
		ID3D11ShaderResourceView* depthSRV = nullptr;
		ID3D11ShaderResourceView* atlas = nullptr;
		ID3D11ShaderResourceView* atlasN = nullptr;  // normal and rough atlases (t3, t4)
		ID3D11ShaderResourceView* atlasR = nullptr;
		std::array<ID3D11ShaderResourceView*, 3> refl{};  // refl_calm / refl_normal / refl_rough (t7..t9)
		ID3D11ShaderResourceView* tidesSrv = nullptr;  // a Surface Tides-style field (t5) and patch (b1)
		ID3D11Buffer*             tidesCb = nullptr;
		ID3D11PixelShader*        curvPS = nullptr;   // the plugin's curvature pass
		ID3D11RenderTargetView*   curvRTV = nullptr;
		ID3D11ShaderResourceView* curvSRV = nullptr;
		int                       curvSize = 0;
		ID3D11VertexShader*       sceneVS = nullptr;
		ID3D11PixelShader*        scenePS = nullptr;
		ID3D11InputLayout*        layout = nullptr;
		ID3D11Buffer*             vb = nullptr;
		ID3D11Buffer*             sceneCB = nullptr;
		ID3D11VertexShader*       passVS = nullptr;
		ID3D11PixelShader*        passPS = nullptr;
		ID3D11Buffer*             passCB = nullptr;
		ID3D11BlendState*         blend = nullptr;
		ID3D11DepthStencilState*  depthOff = nullptr;
		ID3D11RasterizerState*    raster = nullptr;
		ID3D11RasterizerState*    sceneRaster = nullptr;
		ID3D11SamplerState*       sampler = nullptr;
		UINT                      vertexCount = 0;
	};

	bool Setup(Gpu& g, const char* ddsPath, bool hardware = false)
	{
		if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
			std::printf("no WARP device\n");
			return false;
		}
		D3D11_TEXTURE2D_DESC td{};
		td.Width = W;
		td.Height = H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.SampleDesc.Count = 1;
		td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // the game's main target is a float format too
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		g.dev->CreateTexture2D(&td, nullptr, &g.color);
		g.dev->CreateRenderTargetView(g.color, nullptr, &g.colorRTV);
		td.Format = DXGI_FORMAT_R32_UINT;
		g.dev->CreateTexture2D(&td, nullptr, &g.idTex);
		g.dev->CreateRenderTargetView(g.idTex, nullptr, &g.idRTV);
		td.Format = DXGI_FORMAT_R32_TYPELESS;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		g.dev->CreateTexture2D(&td, nullptr, &g.depth);
		D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
		dd.Format = DXGI_FORMAT_D32_FLOAT;
		dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		g.dev->CreateDepthStencilView(g.depth, &dd, &g.dsv);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = DXGI_FORMAT_R32_FLOAT;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		g.dev->CreateShaderResourceView(g.depth, &sd, &g.depthSRV);

		int aw = 0, ah = 0, am = 0;
		if (!LoadDDS(g.dev, ddsPath, &g.atlas, aw, ah, am)) {
			std::printf("could not load %s as a BC7 DDS\n", ddsPath);
			return false;
		}
		std::printf("atlas %s: %dx%d, %d mips\n", ddsPath, aw, ah, am);
		std::string other(ddsPath);
		const auto  slash = other.find_last_of("/\\");
		const std::string dir = slash == std::string::npos ? std::string() : other.substr(0, slash + 1);
		if (!LoadDDS(g.dev, (dir + "normal.dds").c_str(), &g.atlasN, aw, ah, am) || !LoadDDS(g.dev, (dir + "rough.dds").c_str(), &g.atlasR, aw, ah, am)) {
			std::printf("could not load normal.dds / rough.dds next to %s\n", ddsPath);
			return false;
		}
		for (int i = 0; i < 3; ++i) {
			const char* names[3]{ "refl_calm.dds", "refl_normal.dds", "refl_rough.dds" };
			if (!LoadDDS(g.dev, (dir + names[i]).c_str(), &g.refl[static_cast<std::size_t>(i)], aw, ah, am)) {
				std::printf("could not load %s next to %s\n", names[i], ddsPath);
				return false;
			}
		}

		ID3DBlob* vsb = nullptr;
		ID3DBlob* psb = nullptr;
		ID3DBlob* err = nullptr;
		if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
			FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
			std::printf("scene shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
			return false;
		}
		g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.sceneVS);
		g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.scenePS);
		const D3D11_INPUT_ELEMENT_DESC il[]{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "ALBEDO", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "SURFACE", 0, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		g.dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g.layout);
		vsb->Release();
		psb->Release();

		const auto  verts = BuildScene();
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(Vertex));
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init{ verts.data(), 0, 0 };
		g.dev->CreateBuffer(&bd, &init, &g.vb);
		g.vertexCount = static_cast<UINT>(verts.size());
		bd = {};
		bd.ByteWidth = 80;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		g.dev->CreateBuffer(&bd, nullptr, &g.sceneCB);
		bd.ByteWidth = static_cast<UINT>(kPassConstantsSize);
		g.dev->CreateBuffer(&bd, nullptr, &g.passCB);

		// the plugin's pass objects, created the same way as DrawPass.cpp
		g.dev->CreateVertexShader(g_CausticPassVS, sizeof(g_CausticPassVS), nullptr, &g.passVS);
		g.dev->CreatePixelShader(g_CausticPassPS, sizeof(g_CausticPassPS), nullptr, &g.passPS);
		D3D11_BLEND_DESC bl{};
		auto&            rt = bl.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ZERO;
		rt.DestBlend = D3D11_BLEND_SRC_COLOR;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		g.dev->CreateBlendState(&bl, &g.blend);
		D3D11_DEPTH_STENCIL_DESC ds{};
		ds.DepthEnable = FALSE;
		g.dev->CreateDepthStencilState(&ds, &g.depthOff);
		D3D11_RASTERIZER_DESC rs{};
		rs.FillMode = D3D11_FILL_SOLID;
		rs.CullMode = D3D11_CULL_NONE;
		rs.DepthClipEnable = TRUE;
		rs.ScissorEnable = TRUE;
		g.dev->CreateRasterizerState(&rs, &g.raster);
		rs.ScissorEnable = FALSE;
		g.dev->CreateRasterizerState(&rs, &g.sceneRaster);
		D3D11_SAMPLER_DESC sm{};
		sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sm.MaxLOD = D3D11_FLOAT32_MAX;
		g.dev->CreateSamplerState(&sm, &g.sampler);
		return g.passVS && g.passPS && g.blend && g.depthOff && g.raster && g.sampler;
	}

	void DrawScene(Gpu& g, const Camera& cam)
	{
		const float clear[4]{ 0.02f, 0.02f, 0.03f, 1.0f };
		const UINT  zero[4]{ 0, 0, 0, 0 };
		g.ctx->ClearRenderTargetView(g.colorRTV, clear);
		g.ctx->ClearRenderTargetView(g.idRTV, reinterpret_cast<const float*>(zero));
		g.ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
		struct
		{
			float m[16];
			float eye[4];
		} cb{};
		std::memcpy(cb.m, cam.viewProj.m, sizeof(cb.m));
		cb.eye[0] = cam.eye[0];
		cb.eye[1] = cam.eye[1];
		cb.eye[2] = cam.eye[2];
		g.ctx->UpdateSubresource(g.sceneCB, 0, nullptr, &cb, 0, 0);
		ID3D11RenderTargetView* rts[2]{ g.colorRTV, g.idRTV };
		g.ctx->OMSetRenderTargets(2, rts, g.dsv);
		g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		g.ctx->OMSetDepthStencilState(nullptr, 0);
		g.ctx->RSSetState(g.sceneRaster);
		const D3D11_VIEWPORT vp{ 0, 0, W, H, 0, 1 };
		g.ctx->RSSetViewports(1, &vp);
		const UINT stride = sizeof(Vertex), offset = 0;
		g.ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
		g.ctx->IASetInputLayout(g.layout);
		g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g.ctx->VSSetShader(g.sceneVS, nullptr, 0);
		g.ctx->VSSetConstantBuffers(0, 1, &g.sceneCB);
		g.ctx->PSSetShader(g.scenePS, nullptr, 0);
		g.ctx->Draw(g.vertexCount, 0);
		g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
	}

	// The zone's water mask as a texture (same layout as DrawPass::MaskSRV).
	ID3D11ShaderResourceView* MaskView(Gpu& g, const ZoneGpuInput& z)
	{
		static const WaterMask*          s_key = nullptr;
		static ID3D11ShaderResourceView* s_srv = nullptr;
		if (z.mask.get() == s_key) {
			return s_srv;
		}
		Release(s_srv);
		s_key = z.mask.get();
		if (!z.mask) {
			return nullptr;
		}
		const auto&          m = *z.mask;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = static_cast<UINT>(m.size);
		td.MipLevels = static_cast<UINT>(m.mips.size());
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // as DrawPass::MaskSRV: r water, g/b/a water light 0/1/2 reaches
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		static std::vector<std::vector<std::uint8_t>> packed;
		packed.assign(m.mips.size(), {});
		std::vector<D3D11_SUBRESOURCE_DATA> init(m.mips.size());
		for (std::size_t l = 0; l < m.mips.size(); ++l) {
			const auto& r = m.mips[l];
			packed[l].resize(r.size() * 4);
			for (std::size_t i = 0; i < r.size(); ++i) {
				packed[l][4 * i] = r[i];
				for (std::size_t k = 0; k < 3; ++k) {
					packed[l][4 * i + 1 + k] = m.litMips[k].size() == m.mips.size() ? m.litMips[k][l][i] : r[i];
				}
			}
			init[l] = { packed[l].data(), static_cast<UINT>(std::max(1, m.size >> l) * 4), 0 };
		}
		ID3D11Texture2D* tex = nullptr;
		if (SUCCEEDED(g.dev->CreateTexture2D(&td, init.data(), &tex))) {
			g.dev->CreateShaderResourceView(tex, nullptr, &s_srv);
			tex->Release();
		}
		return s_srv;
	}

	// The Cycles reference's waves (tools/blender-caustic-ref.py, scaled 100: amplitude in units, wavelength
	// 168) as a Surface Tides field: 192 x 192 cells of 20 around the origin, float4 (height, 0, 0, valid).
	TidesField MakeTides(float amplitude)
	{
		TidesField f;
		f.resolution = 192;
		f.spacing = 20.0f;
		f.originX = f.originY = -1920.0f;
		f.height.resize(192 * 192);
		f.valid.assign(192 * 192, 1.0f);
		const float k = 6.2831853f / 168.0f, ph[3]{ 0.0f, 1.3f, 2.1f };
		for (int y = 0; y < 192; ++y) {
			for (int x = 0; x < 192; ++x) {
				const float wx = f.originX + x * f.spacing, wy = f.originY + y * f.spacing;
				float       h = 0.0f;
				for (int i = 0; i < 3; ++i) {
					const float a = i * 60.0f * 3.14159265f / 180.0f;
					h += amplitude * std::sin(k * (std::cos(a) * wx + std::sin(a) * wy) + ph[i]);
				}
				f.height[static_cast<size_t>(y) * 192 + x] = h;
			}
		}
		return f;
	}

	void UploadTides(Gpu& g, const TidesField& f)
	{
		Release(g.tidesSrv);
		Release(g.tidesCb);
		std::vector<float> texels(static_cast<size_t>(f.resolution) * f.resolution * 4);
		for (size_t i = 0; i < f.height.size(); ++i) {
			texels[i * 4] = f.height[i];
			texels[i * 4 + 3] = f.valid[i];
		}
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = static_cast<UINT>(f.resolution);
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{ texels.data(), static_cast<UINT>(f.resolution * 16), 0 };
		ID3D11Texture2D*       tex = nullptr;
		g.dev->CreateTexture2D(&td, &init, &tex);
		g.dev->CreateShaderResourceView(tex, nullptr, &g.tidesSrv);
		tex->Release();
		const float cb[20]{ f.originX, f.originY, f.spacing, f.rest, f.originX, f.originY, f.spacing, f.rest, static_cast<float>(f.resolution), 1.0f,
			f.displacement, 0.0f, f.enabled, 1.0f, f.edgeCells, f.tolerance, 0, 0, 0, 0 };
		D3D11_BUFFER_DESC      bd{};
		bd.ByteWidth = sizeof(cb);
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA cinit{ cb, 0, 0 };
		g.dev->CreateBuffer(&bd, &cinit, &g.tidesCb);

		Release(g.curvRTV);
		Release(g.curvSRV);
		if (!g.curvPS) {
			g.dev->CreatePixelShader(g_CausticCurvPS, sizeof(g_CausticCurvPS), nullptr, &g.curvPS);
		}
		D3D11_TEXTURE2D_DESC ct{};
		ct.Width = ct.Height = static_cast<UINT>(f.resolution);
		ct.MipLevels = 1;
		ct.ArraySize = 1;
		ct.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		ct.SampleDesc.Count = 1;
		ct.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* curv = nullptr;
		g.dev->CreateTexture2D(&ct, nullptr, &curv);
		g.dev->CreateRenderTargetView(curv, nullptr, &g.curvRTV);
		g.dev->CreateShaderResourceView(curv, nullptr, &g.curvSRV);
		curv->Release();
		g.curvSize = f.resolution;
	}

	// The shader's PathVisible (the reflected path's occlusion test) over the rendered scene depth.
	struct ScreenOcclusion final : PathOcclusion
	{
		const std::vector<float>* depth = nullptr;
		Mat4                      viewProj, invViewProj;
		Vec3                      posAdjust{};
		float Visible(const Vec3& from, const Vec3& to) const noexcept override
		{
			for (int k = 1; k <= kOcclusionSteps; ++k) {
				const float f = 0.05f + 0.9f * k / (kOcclusionSteps + 1);
				const Vec3  xr{ to[0] + (from[0] - to[0]) * f - posAdjust[0], to[1] + (from[1] - to[1]) * f - posAdjust[1], to[2] + (from[2] - to[2]) * f - posAdjust[2] };
				const Vec4  clip = TransformRow({ xr[0], xr[1], xr[2], 1.0f }, viewProj);
				if (clip[3] <= 1.0f) {
					continue;
				}
				const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
				if (std::fabs(nx) >= 1.0f || std::fabs(ny) >= 1.0f) {
					continue;
				}
				const int   px = static_cast<int>((nx * 0.5f + 0.5f) * W), py = static_cast<int>((0.5f - ny * 0.5f) * H);
				const float d = (*depth)[static_cast<size_t>(std::clamp(py, 0, H - 1)) * W + std::clamp(px, 0, W - 1)];
				const Vec4  hs = TransformRow({ nx, ny, d, 1.0f }, invViewProj);
				const float hw = std::fabs(hs[3]) > 1e-6f ? hs[3] : 1e-6f;
				const float ds = std::sqrt((hs[0] / hw) * (hs[0] / hw) + (hs[1] / hw) * (hs[1] / hw) + (hs[2] / hw) * (hs[2] / hw));
				const float dx = std::sqrt(xr[0] * xr[0] + xr[1] * xr[1] + xr[2] * xr[2]);
				if (ds > kOcclusionNear && ds < dx - kOcclusionBias && ds > dx - kOcclusionThickness) {
					return 0.0f;
				}
			}
			return 1.0f;
		}
	};

	// Which water texels the lamp reaches: the segment from the texel centre (on the water plane) to the
	// lamp must miss the bridge slab (x -230..230, y -40..40, z 120..135).
	std::vector<std::uint8_t> BridgeLit(const WaterMask& m, const Vec3& L)
	{
		std::vector<std::uint8_t> lit(static_cast<size_t>(m.size) * m.size, 0);
		const float tx = (m.bounds.maxX - m.bounds.minX) / m.size, ty = (m.bounds.maxY - m.bounds.minY) / m.size;
		const float lo[3]{ -230, -40, 120 }, hi[3]{ 230, 40, 135 };
		for (int y = 0; y < m.size; ++y) {
			for (int x = 0; x < m.size; ++x) {
				const float o[3]{ m.bounds.minX + (x + 0.5f) * tx, m.bounds.minY + (y + 0.5f) * ty, 0.0f };
				const float d[3]{ L[0] - o[0], L[1] - o[1], L[2] - o[2] };
				float       t0 = 0.0f, t1 = 1.0f;
				bool        hit = true;
				for (int a = 0; a < 3 && hit; ++a) {
					if (std::fabs(d[a]) < 1e-6f) {
						hit = o[a] >= lo[a] && o[a] <= hi[a];
						continue;
					}
					float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
					if (ta > tb) {
						std::swap(ta, tb);
					}
					t0 = std::max(t0, ta);
					t1 = std::min(t1, tb);
					hit = t0 <= t1;
				}
				lit[static_cast<size_t>(y) * m.size + x] = hit ? 0 : 1;
			}
		}
		return lit;
	}

	// Same steps as DrawPass::Render for one zone.
	bool DrawPass(Gpu& g, const Camera& cam, const ZoneGpuInput& z)
	{
		FrameCamera fc;
		fc.invViewProj = cam.invViewProj;
		fc.viewProj = cam.viewProj;
		fc.posAdjust = cam.eye;
		fc.viewport = { 0, 0, W, H, 0, 1 };
		const auto sc = ScissorForZone(z.rect, z.waterZ - kBottomMaxDepth, z.height + kBottomMaxDepth, z.height, fc.posAdjust, cam.viewProj, fc.viewport);
		if (!sc) {
			std::printf("  (zone off screen)\n");
			return false;
		}
		const PassConstants pc = PackPass(z, fc);
		g.ctx->UpdateSubresource(g.passCB, 0, nullptr, &pc, 0, 0);
		g.ctx->OMSetRenderTargets(1, &g.colorRTV, nullptr);
		const FLOAT factor[4]{ 1, 1, 1, 1 };
		g.ctx->OMSetBlendState(g.blend, factor, 0xFFFFFFFF);
		g.ctx->OMSetDepthStencilState(g.depthOff, 0);
		g.ctx->RSSetState(g.raster);
		const D3D11_RECT r{ sc->left, sc->top, sc->right, sc->bottom };
		g.ctx->RSSetScissorRects(1, &r);
		g.ctx->IASetInputLayout(nullptr);
		g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g.ctx->VSSetShader(g.passVS, nullptr, 0);
		g.ctx->PSSetShader(g.passPS, nullptr, 0);
		g.ctx->PSSetConstantBuffers(0, 1, &g.passCB);
		g.ctx->PSSetSamplers(0, 1, &g.sampler);
		if (z.tides) {  // as DrawPass::Render: the field's curvature first
			g.ctx->PSSetShaderResources(5, 1, &g.tidesSrv);
			g.ctx->PSSetConstantBuffers(1, 1, &g.tidesCb);
			g.ctx->OMSetRenderTargets(1, &g.curvRTV, nullptr);
			g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
			const D3D11_VIEWPORT cvp{ 0.0f, 0.0f, static_cast<float>(g.curvSize), static_cast<float>(g.curvSize), 0.0f, 1.0f };
			const D3D11_RECT     crect{ 0, 0, g.curvSize, g.curvSize };
			g.ctx->RSSetViewports(1, &cvp);
			g.ctx->RSSetScissorRects(1, &crect);
			g.ctx->PSSetShader(g.curvPS, nullptr, 0);
			g.ctx->Draw(3, 0);
			g.ctx->OMSetRenderTargets(1, &g.colorRTV, nullptr);
			g.ctx->OMSetBlendState(g.blend, factor, 0xFFFFFFFF);
			const D3D11_VIEWPORT vp{ 0, 0, W, H, 0, 1 };
			g.ctx->RSSetViewports(1, &vp);
			g.ctx->RSSetScissorRects(1, &r);
			g.ctx->PSSetShader(g.passPS, nullptr, 0);
		}
		ID3D11ShaderResourceView* srvs[10]{ g.depthSRV, g.atlas, MaskView(g, z), g.atlasN, g.atlasR, z.tides ? g.tidesSrv : nullptr, z.tides ? g.curvSRV : nullptr,
			g.refl[0], g.refl[1], g.refl[2] };
		g.ctx->PSSetShaderResources(0, 10, srvs);
		g.ctx->Draw(3, 0);
		ID3D11ShaderResourceView* none[10]{};
		g.ctx->PSSetShaderResources(0, 10, none);
		g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
		return true;
	}

	template <class T>
	std::vector<T> Read(Gpu& g, ID3D11Texture2D* src, DXGI_FORMAT fmt, int comps, bool half)
	{
		D3D11_TEXTURE2D_DESC td{};
		src->GetDesc(&td);
		td.Format = fmt == DXGI_FORMAT_UNKNOWN ? td.Format : fmt;
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D* st = nullptr;
		g.dev->CreateTexture2D(&td, nullptr, &st);
		g.ctx->CopyResource(st, src);
		D3D11_MAPPED_SUBRESOURCE m{};
		g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
		std::vector<T> out(static_cast<size_t>(W) * H * comps);
		for (int y = 0; y < H; ++y) {
			const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
			for (int x = 0; x < W; ++x) {
				for (int c = 0; c < comps; ++c) {
					if constexpr (std::is_same_v<T, float>) {
						if (half) {
							std::uint16_t hv;
							std::memcpy(&hv, row + (x * 4 + c) * 2, 2);  // RGBA16F, keep RGB
							const std::uint32_t s = (hv >> 15) & 1, e = (hv >> 10) & 31, f = hv & 1023;
							float               v = e == 0 ? std::ldexp(static_cast<float>(f), -24) : std::ldexp(static_cast<float>(f | 1024), static_cast<int>(e) - 25);
							out[(static_cast<size_t>(y) * W + x) * comps + c] = s ? -v : v;
						} else {
							std::memcpy(&out[(static_cast<size_t>(y) * W + x) * comps + c], row + (x * comps + c) * 4, 4);
						}
					} else {
						std::memcpy(&out[(static_cast<size_t>(y) * W + x) * comps + c], row + (x * comps + c) * 4, 4);
					}
				}
			}
		}
		g.ctx->Unmap(st, 0);
		st->Release();
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::printf("usage: PassRender <calm.dds> <out dir>\n");
		return 2;
	}
	const std::string out = argv[2];
	const bool        bench = argc > 3 && std::string(argv[3]) == "--bench";
	Gpu               g;
	if (!Setup(g, argv[1], bench)) {
		return 1;
	}

	ZoneGpuInput z;
	z.mask = MaskFromRect({ -200, -200, 200, 200 });
	z.rect = z.mask->bounds;
	z.waterZ = 0;
	z.height = 250;
	z.distFade = 1;
	z.light = { 600, 600, 450 };
	z.lightRadius = 1000;
	z.lightColor = { 0.77f, 0.77f, 0.77f };
	z.level = 1;
	z.brightness = 1;
	z.frameClock = 10.0f;

	// eye over the north-east of the pond, below the bridge, looking south-west at the lit walls
	const Camera cam = MakeCamera({ 150, 120, 60 }, { -230, -200, 110 });

	if (bench) {
		// GPU time of the pass on the real graphics card: three lights, spread, the zone filling the view
		ZoneGpuInput zb = z;
		zb.strength = kDefaultStrength;
		zb.spread = ReflectionSpread(0.5f);
		zb.extra[0] = { { -150, 150, 200 }, 500, { 1.0f, 0.6f, 0.25f }, 0.5f };
		zb.extra[1] = { { 100, -150, 180 }, 400, { 1.0f, 0.7f, 0.4f }, 0.4f };
		DrawScene(g, cam);
		D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt{ D3D11_QUERY_TIMESTAMP, 0 };
		ID3D11Query *dj = nullptr, *t0 = nullptr, *t1 = nullptr;
		g.dev->CreateQuery(&qd, &dj);
		g.dev->CreateQuery(&qt, &t0);
		g.dev->CreateQuery(&qt, &t1);
		constexpr int kRuns = 200;
		for (int warm = 0; warm < 20; ++warm) {
			DrawPass(g, cam, zb);
		}
		g.ctx->Begin(dj);
		g.ctx->End(t0);
		for (int i = 0; i < kRuns; ++i) {
			DrawPass(g, cam, zb);
		}
		g.ctx->End(t1);
		g.ctx->End(dj);
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
		while (g.ctx->GetData(dj, &d, sizeof(d), 0) != S_OK) {
		}
		UINT64 a = 0, b = 0;
		g.ctx->GetData(t0, &a, sizeof(a), 0);
		g.ctx->GetData(t1, &b, sizeof(b), 0);
		const double ms = 1000.0 * static_cast<double>(b - a) / static_cast<double>(d.Frequency) / kRuns;
		std::printf("[bench] hardware GPU: %.4f ms per pass at %dx%d (whole view in the zone, 3 lights)%s\n", ms, W, H, d.Disjoint ? " (disjoint!)" : "");
		std::printf("[bench] pixel-bound estimate: %.3f ms at 1280x800 (the game's 50 %% render), %.3f ms at 2560x1600\n", ms * 4.0, ms * 16.0);
		// the Surface Tides path: the Cycles ripples as a borrowed field
		UploadTides(g, MakeTides(2.3f));
		zb.tides = true;
		zb.tidesSize = 192.0f;  // MakeTides: 192 x 192 cells
		zb.mask = WithLitPlane(*zb.mask, BridgeLit(*zb.mask, zb.light));
		zb.useLit = true;
		for (int warm = 0; warm < 20; ++warm) {
			DrawPass(g, cam, zb);
		}
		g.ctx->Begin(dj);
		g.ctx->End(t0);
		for (int i = 0; i < kRuns; ++i) {
			DrawPass(g, cam, zb);
		}
		g.ctx->End(t1);
		g.ctx->End(dj);
		while (g.ctx->GetData(dj, &d, sizeof(d), 0) != S_OK) {
		}
		g.ctx->GetData(t0, &a, sizeof(a), 0);
		g.ctx->GetData(t1, &b, sizeof(b), 0);
		const double msT = 1000.0 * static_cast<double>(b - a) / static_cast<double>(d.Frequency) / kRuns;
		std::printf("[bench] with the Surface Tides field: %.4f ms per pass at %dx%d -> %.3f ms at 1280x800\n", msT, W, H, msT * 4.0);
		return 0;
	}

	DrawScene(g, cam);
	const auto base = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	const auto ids = Read<std::uint32_t>(g, g.idTex, DXGI_FORMAT_UNKNOWN, 1, false);
	const auto depth = Read<float>(g, g.depth, DXGI_FORMAT_R32_FLOAT, 1, false);
	WritePPM(out + "/pass_base.ppm", base);
	ScreenOcclusion occ;
	occ.depth = &depth;
	occ.viewProj = cam.viewProj;
	occ.invViewProj = cam.invViewProj;
	occ.posAdjust = cam.eye;

	// ---- 1. light mask vs the C++ mirror --------------------------------------------------------------
	std::printf("[light mask]\n");
	ZoneGpuInput zm = z;
	zm.debugLight = true;
	DrawScene(g, cam);
	Check(DrawPass(g, cam, zm), "zone on screen");
	const auto mask = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	WritePPM(out + "/pass_mask.ppm", mask);

	FrameCamera fc;
	fc.invViewProj = cam.invViewProj;
		fc.viewProj = cam.viewProj;
	fc.posAdjust = cam.eye;
	fc.viewport = { 0, 0, W, H, 0, 1 };
	const PassConstants pcm = PackPass(zm, fc);
	auto                world = [&](int x, int y) -> std::optional<Vec3> {
        const auto r = UnprojectPixel(x + 0.5f, y + 0.5f, depth[static_cast<size_t>(y) * W + x], fc.viewport, fc.invViewProj);
        if (!r) {
            return std::nullopt;
        }
        return Vec3{ (*r)[0] + fc.posAdjust[0], (*r)[1] + fc.posAdjust[1], (*r)[2] + fc.posAdjust[2] };
	};
	const Vec3 faceN[kSurfaces]{ { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 0, 1, 0 } };  // sides: only the north one is visible

	std::array<double, kSurfaces> sumMask{}, sumBoost{};
	std::array<int, kSurfaces>    count{}, litMask{}, litBoost{};
	int                           compared = 0, agree = 0;
	double                        worst = 0;
	for (int y = 1; y < H - 1; ++y) {
		for (int x = 1; x < W - 1; ++x) {
			const size_t i = static_cast<size_t>(y) * W + x;
			const auto   s = ids[i];
			if (s == kNone || s >= kSurfaces) {
				continue;
			}
			// skip pixels next to another surface (the shader's ddx/ddy normal is wrong at edges)
			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if (ids[i + dy * W + dx] != s) {
						edge = true;
						break;
					}
				}
			}
			const float got = base[i * 3 + 2] > 1e-4f ? mask[i * 3 + 2] / base[i * 3 + 2] - 1.0f : 0.0f;
			++count[s];
			sumMask[s] += got;
			litMask[s] += got > 0.02f;
			if (edge) {
				continue;
			}
			const auto p = world(x, y);
			if (!p) {
				continue;
			}
			const float want = CausticBoost(pcm, *p, faceN[s], 1.0f, z.mask.get(), nullptr, &occ).add.b;
			++compared;
			const double d = std::fabs(got - want);
			worst = std::max(worst, d);
			agree += d < 0.01 + 0.01 * want;  // half-float target: ~0.1 % steps
		}
	}
	std::printf("  shader vs C++ mirror: %d of %d interior pixels agree (worst difference %.4f)\n", agree, compared, worst);
	Check(compared > 1000 && agree >= compared * 99 / 100, "light mask matches the C++ mirror on 99 % of pixels", agree, compared);
	for (int s = 1; s < kSurfaces; ++s) {
		if (count[s]) {
			std::printf("  %-22s %6d px, lit %5.1f %%, mean +%.3f\n", kSurfaceNames[s], count[s], 100.0 * litMask[s] / count[s], sumMask[s] / count[s]);
		}
	}
	auto frac = [&](Surface s, const std::array<int, kSurfaces>& lit) { return count[s] ? static_cast<double>(lit[s]) / count[s] : -1.0; };
	Check(frac(kBridgeUnder, litMask) > 0.3, "bridge underside is lit", frac(kBridgeUnder, litMask), 0.3);
	Check(frac(kWallSW_X, litMask) > 0.1 || frac(kWallSW_Y, litMask) > 0.1, "a south-west wall is lit",
		std::max(frac(kWallSW_X, litMask), frac(kWallSW_Y, litMask)), 0.1);
	
	Check(frac(kPondBottom, litMask) > 0.3, "pond bottom gets the light refracted through the water", frac(kPondBottom, litMask), 0.3);

	// ---- 2. caustics at the default strength --------------------------------------------------------
	std::printf("[caustics, strength %.0f]\n", kDefaultStrength);
	ZoneGpuInput zc = z;
	zc.strength = kDefaultStrength;
	DrawScene(g, cam);
	DrawPass(g, cam, zc);
	const auto caus = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	WritePPM(out + "/pass_caustic.ppm", caus);
	double maxBoost = 0, minBoost = 0, minAbove = 0;
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const size_t i = static_cast<size_t>(y) * W + x;
			const auto   s = ids[i];
			if (s == kNone || s >= kSurfaces || base[i * 3] <= 1e-4f) {
				continue;
			}
			const double b = caus[i * 3] / base[i * 3] - 1.0;
			maxBoost = std::max(maxBoost, b);
			minBoost = std::min(minBoost, b);
			if (s != kPondBottom) {
				minAbove = std::min(minAbove, b);
			}
			sumBoost[s] += b;
			litBoost[s] += b > 0.03;
		}
	}
	for (int s = 1; s < kSurfaces; ++s) {
		if (count[s]) {
			std::printf("  %-22s visible boost (>3 %%) on %5.1f %%, mean +%.1f %%\n", kSurfaceNames[s], 100.0 * litBoost[s] / count[s],
				100.0 * sumBoost[s] / count[s]);
		}
	}
	std::printf("  boost range %.3f .. %.3f\n", minBoost, maxBoost);
	Check(maxBoost > 0.08, "caustics are visible somewhere (> +8 %)", maxBoost, 0.08);
	Check(maxBoost <= kMaxBoost + 0.005, "never above the cap", maxBoost, kMaxBoost);
	Check(minAbove > -0.002, "above the water never darker than the scene", minAbove, 0);
	Check(minBoost > kMinBottomBoost - 0.005, "the pool floor at most 30 % darker between the streaks", minBoost, kMinBottomBoost);
	Check(std::fabs(sumBoost[kPondBottom] / std::max(count[kPondBottom], 1)) < 0.03, "the pool floor keeps its mean brightness (redistribution)", sumBoost[kPondBottom] / std::max(count[kPondBottom], 1), 0);
	Check(frac(kBridgeUnder, litBoost) > 0.05, "bridge underside shows pattern", frac(kBridgeUnder, litBoost), 0.05);

	// ---- 3. rippled water (glossy spread) + a second, orange lantern --------------------------------
	std::printf("[glossy spread + second light]\n");
	ZoneGpuInput zg = z;
	zg.spread = ReflectionSpread(0.5f);
	zg.extra[0] = { { -150, 150, 200 }, 500, { 1.0f, 0.6f, 0.25f }, 0.5f };  // a weaker lantern north-west
	ZoneGpuInput zgm = zg;
	zgm.debugLight = true;
	ZoneGpuInput zsm = zgm;  // same two lights, no spread: the fair baseline
	zsm.spread = 0.0f;
	DrawScene(g, cam);
	DrawPass(g, cam, zsm);
	const auto smask = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	DrawScene(g, cam);
	DrawPass(g, cam, zgm);
	const auto gmask = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	WritePPM(out + "/pass_glossy_mask.ppm", gmask);
	const PassConstants pcg = PackPass(zgm, fc);
	int    gCompared = 0, gAgree = 0, gLitSharp = 0, gLitGlossy = 0;
	double gWorst = 0;
	for (int y = 1; y < H - 1; ++y) {
		for (int x = 1; x < W - 1; ++x) {
			const size_t i = static_cast<size_t>(y) * W + x;
			const auto   s = ids[i];
			if (s == kNone || s >= kSurfaces) {
				continue;
			}
			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if (ids[i + dy * W + dx] != s) {
						edge = true;
						break;
					}
				}
			}
			const float got = base[i * 3 + 2] > 1e-4f ? gmask[i * 3 + 2] / base[i * 3 + 2] - 1.0f : 0.0f;
			gLitGlossy += got > 0.02f;
			gLitSharp += (base[i * 3 + 2] > 1e-4f ? smask[i * 3 + 2] / base[i * 3 + 2] - 1.0f : 0.0f) > 0.02f;
			if (edge) {
				continue;
			}
			const auto wp = world(x, y);
			if (!wp) {
				continue;
			}
			const float want = CausticBoost(pcg, *wp, faceN[s], 1.0f, zgm.mask.get(), nullptr, &occ).add.b;
			++gCompared;
			const double d = std::fabs(got - want);
			gWorst = std::max(gWorst, d);
			gAgree += d < 0.03;  // trilinear filtering on the GPU uses fixed-point weights
		}
	}
	std::printf("  shader vs C++ mirror (mip-sampled mask, two lights): %d of %d agree (worst %.4f)\n", gAgree, gCompared, gWorst);
	std::printf("  lit pixels: mirror-sharp %d, glossy %d\n", gLitSharp, gLitGlossy);
	Check(gCompared > 1000 && gAgree >= gCompared * 99 / 100, "glossy light mask matches the C++ mirror on 99 % of pixels", gAgree, gCompared);
	Check(gLitGlossy > gLitSharp, "rippled water spreads the reflected light over more of the room", gLitGlossy, gLitSharp);

	ZoneGpuInput zgc = zg;
	zgc.strength = kDefaultStrength;
	DrawScene(g, cam);
	DrawPass(g, cam, zgc);
	const auto gcaus = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
	WritePPM(out + "/pass_glossy_caustic.ppm", gcaus);
	double gMax = 0, gMin = 0;
	int    gVisible = 0;
	for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
		if (ids[i] == kNone || base[i * 3] <= 1e-4f) {
			continue;
		}
		const double bst = gcaus[i * 3] / base[i * 3] - 1.0;
		gMax = std::max(gMax, bst);
		gMin = std::min(gMin, bst);
		gVisible += bst > 0.03;
	}
	std::printf("  glossy caustics: visible (>3 %%) on %d px, boost range %.3f .. %.3f\n", gVisible, gMin, gMax);
	Check(gMax <= kMaxBoost + 0.005 && gMin > kMinBottomBoost - 0.005, "glossy caustics stay within the floor minimum .. the cap", gMax, kMaxBoost);
	Check(gVisible > 0, "glossy caustics are visible", gVisible, 0);

	// ---- 3a. pattern layers and swimmer rings -------------------------------------------------------
	std::printf("[layers + ripple]\n");
	{
		auto render = [&](const ZoneGpuInput& zz) {
			DrawScene(g, cam);
			DrawPass(g, cam, zz);
			return Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		};
		auto mean = [&](const std::vector<float>& img) {
			double sum = 0;
			int    n = 0;
			for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
				if (ids[i] == kWallSW_Y && base[i * 3] > 1e-4f) {
					sum += img[i * 3] / base[i * 3] - 1.0;
					++n;
				}
			}
			return n ? sum / n : 0.0;
		};
		ZoneGpuInput zl = z;
		zl.strength = kDefaultStrength;
		zl.weights = { 1.0f, 0.0f, 0.0f };
		const double calmMean = mean(render(zl));
		zl.weights = { 0.0f, 0.0f, 1.0f };
		const auto   roughImg = render(zl);
		const double roughMean = mean(roughImg);
		zl.weights = CrossfadeWeights(0.25f);
		const double mixMean = mean(render(zl));
		std::printf("  south-west wall (y) mean boost: calm %.2f %%, rough %.2f %%, calm/normal mix %.2f %%\n", 100 * calmMean, 100 * roughMean, 100 * mixMean);
		Check(roughMean > 0.3 * calmMean && roughMean < 3.0 * calmMean, "rough atlas rescaled to the calm mean: similar brightness", roughMean, calmMean);
		Check(mixMean > 0.3 * calmMean && mixMean < 3.0 * calmMean, "a crossfade stays in the same range", mixMean, calmMean);

		ZoneGpuInput zq = z;
		zq.strength = kDefaultStrength;
		const auto still = render(zq);
		zq.rippleX = -150.0f;
		zq.rippleY = -150.0f;
		zq.rippleAmp = 1.0f;
		zq.rippleRadius = 150.0f;
		const auto rings = render(zq);
		int changed = 0;
		for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
			changed += std::fabs(rings[i * 3] - still[i * 3]) > 0.002f * std::max(still[i * 3], 1e-3f) + 1e-4f;
		}
		std::printf("  a swimmer at (-150, -150) changes %d pixels\n", changed);
		Check(changed > 1000, "swimmer rings change the caustics near the swimmer", changed, 1000);
		double maxB = 0;
		for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
			if (ids[i] != kNone && base[i * 3] > 1e-4f) {
				maxB = std::max(maxB, static_cast<double>(rings[i * 3] / base[i * 3] - 1.0f));
			}
		}
		Check(maxB <= kMaxBoost + 0.005, "rings stay under the cap", maxB, kMaxBoost);
	}

	// ---- 3c. the bridge's shadow on the water (lit plane), per pixel vs the C++ mirror ----------------
	std::printf("[shadowed water]\n");
	{
		ZoneGpuInput zs = zg;
		zs.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));
		zs.rect = zs.mask->bounds;
		zs.useLit = true;
		std::printf("  lit plane: %d of %d water texels reach the lamp\n", zs.mask->litTexels[0], zs.mask->waterTexels);
		Check(zs.mask->litTexels[0] > 0 && zs.mask->litTexels[0] < zs.mask->waterTexels, "the bridge shadows part of the pool");
		ZoneGpuInput zsm = zs;
		zsm.debugLight = true;
		DrawScene(g, cam);
		DrawPass(g, cam, zsm);
		const auto          img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pcs = PackPass(zsm, fc);
		int                 n = 0, ok = 0, litS = 0, litU = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3 + 2] <= 1e-4f) {
					continue;
				}
				const float got = img[i * 3 + 2] / base[i * 3 + 2] - 1.0f;
				litS += got > 0.02f;
				litU += gmask[i * 3 + 2] / base[i * 3 + 2] - 1.0f > 0.02f;
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				++n;
				ok += std::fabs(got - CausticBoost(pcs, *wp, faceN[s], 1.0f, zsm.mask.get(), nullptr, &occ).add.b) < 0.03;
			}
		}
		std::printf("  shader vs C++ mirror with the lit plane: %d of %d agree; lit pixels %d (unshadowed %d)\n", ok, n, ok ? litS : 0, litU);
		Check(n > 1000 && ok >= n * 99 / 100, "lit-plane light mask matches the C++ mirror on 99 % of pixels", ok, n);
		Check(litS < litU, "the shadowed water lights less of the room", litS, litU);

		// the north-west lantern (extra light 0) with a lit plane of its own: the bridge shadows its water too
		ZoneGpuInput zx = zsm;
		zx.mask = WithLitPlane(*zs.mask, BridgeLit(*z.mask, zg.extra[0].pos), 1);
		zx.useLitExtra[0] = true;
		std::printf("  lantern's lit plane: %d of %d water texels reach it\n", zx.mask->litTexels[1], zx.mask->waterTexels);
		DrawScene(g, cam);
		DrawPass(g, cam, zx);
		const auto          imgX = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pcx = PackPass(zx, fc);
		int                 nX = 0, okX = 0, litX = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3 + 2] <= 1e-4f) {
					continue;
				}
				const float got = imgX[i * 3 + 2] / base[i * 3 + 2] - 1.0f;
				litX += got > 0.02f;
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				++nX;
				okX += std::fabs(got - CausticBoost(pcx, *wp, faceN[s], 1.0f, zx.mask.get(), nullptr, &occ).add.b) < 0.03;
			}
		}
		std::printf("  shader vs C++ mirror with both lights' lit planes: %d of %d agree; lit pixels %d (lantern unshadowed %d)\n", okX, nX, litX, litS);
		Check(zx.mask->litTexels[1] > 0 && zx.mask->litTexels[1] < zx.mask->waterTexels, "the bridge shadows part of the pool from the lantern too");
		Check(nX > 1000 && okX >= nX * 99 / 100, "per-light lit planes match the C++ mirror on 99 % of pixels", okX, nX);
		Check(litX < litS, "the lantern's own shadow takes its light off the walls the bridge hides from it", litX, litS);

		// the lantern while its lit plane is still being cast: it reflects nothing (not everywhere), shader = mirror
		ZoneGpuInput zp = zx;
		zp.useLitExtra[0] = false;
		zp.planePending[1] = true;
		DrawScene(g, cam);
		DrawPass(g, cam, zp);
		const auto          imgP = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pcp = PackPass(zp, fc);
		int                 nP = 0, okP = 0, litP = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3 + 2] <= 1e-4f) {
					continue;
				}
				const float got = imgP[i * 3 + 2] / base[i * 3 + 2] - 1.0f;
				litP += got > 0.02f;
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				++nP;
				okP += std::fabs(got - CausticBoost(pcp, *wp, faceN[s], 1.0f, zp.mask.get(), nullptr, &occ).add.b) < 0.03;
			}
		}
		std::printf("  shader vs C++ mirror with the lantern's plane pending: %d of %d agree; lit pixels %d (its own plane %d)\n", okP, nP, litP, litX);
		Check(nP > 1000 && okP >= nP * 99 / 100, "a pending lit plane matches the C++ mirror on 99 % of pixels", okP, nP);
		Check(litP < litX, "a pending lantern lights no wall (less than with its own plane)", litP, litX);
	}

	// ---- 3d. the Surface Tides path: brightness from the field's curvature, shader vs C++ mirror -------
	std::printf("[surface tides field]\n");
	const TidesField tf = MakeTides(2.3f);  // the Cycles calm ripples
	UploadTides(g, tf);
	{
		ZoneGpuInput zt = z;
		zt.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));
		zt.rect = zt.mask->bounds;
		zt.useLit = true;
		zt.tides = true;
		zt.tidesSize = 192.0f;  // MakeTides: 192 x 192 cells
		zt.tidesFine = 0.0f;  // the output then depends on the field only: comparable per pixel
		zt.strength = kDefaultStrength;
		zt.spread = SpreadFromSlope(SlopeAngle(kSlopeStill));
		DrawScene(g, cam);
		DrawPass(g, cam, zt);
		const auto          img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pt = PackPass(zt, fc);
		int                 n = 0, ok = 0, bright = 0, dark = 0;
		double              worst = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3] <= 1e-4f) {
					continue;
				}
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				const float got = img[i * 3] / base[i * 3] - 1.0f;
				const float want = CausticBoost(pt, *wp, faceN[s], 0.5f, zt.mask.get(), &tf, &occ).add.r;
				++n;
				const double d = std::fabs(got - want);
				ok += d < 0.01 + 0.03 * std::fabs(want);
				worst = std::max(worst, d);
				bright += got > 0.03f;
				dark += got < -0.03f;
			}
		}
		std::printf("  shader vs C++ mirror with the field: %d of %d agree (worst %.4f); brighter %d, darker (floor) %d\n", ok, n, worst, bright, dark);
		Check(n > 1000 && ok >= n * 98 / 100, "tides-field caustics match the C++ mirror on 98 % of pixels", ok, n);
		Check(bright > 1000, "the field's ripples light the walls", bright, 1000);
		Check(dark > 100, "and redistribute the floor light (darker between the bright bands)", dark, 100);
	}

	// ---- 3d'. the same field with a coloured lamp and a coloured room: every channel vs the C++ mirror ---------
	// (the share is taken per channel: a swapped or luminance-only channel in the shader fails here)
	std::printf("[coloured share]\n");
	{
		ZoneGpuInput zc = z;
		zc.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));
		zc.rect = zc.mask->bounds;
		zc.useLit = true;
		zc.tides = true;
		zc.tidesSize = 192.0f;
		zc.tidesFine = 0.0f;
		zc.strength = kDefaultStrength;
		zc.spread = SpreadFromSlope(SlopeAngle(kSlopeStill));
		zc.lightColor = { 1.0f, 0.55f, 0.2f };   // a torch
		zc.level = 0.6f;                          // the key at a partial level (the sun at its beam) ...
		zc.extra[0] = { { -150, 150, 200 }, 500, { 1.0f, 0.8f, 0.6f }, 0.5f * zc.level };  // ... and an extra at its own (OutputTick scales by the level)
		zc.ambient = 0.3f;
		zc.ambientHue = { 0.6f, 0.9f, 1.4f };      // a bluish room
		DrawScene(g, cam);
		DrawPass(g, cam, zc);
		const auto          img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pc = PackPass(zc, fc);
		int                 n = 0, ok = 0, warmer = 0;
		double              worst = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3] <= 1e-4f || base[i * 3 + 1] <= 1e-4f || base[i * 3 + 2] <= 1e-4f) {
					continue;
				}
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				const auto  want = CausticBoost(pc, *wp, faceN[s], 0.5f, zc.mask.get(), &tf, &occ).add;
				const float gr = img[i * 3] / base[i * 3] - 1.0f, gg = img[i * 3 + 1] / base[i * 3 + 1] - 1.0f, gb = img[i * 3 + 2] / base[i * 3 + 2] - 1.0f;
				const double d = std::max({ std::fabs(gr - want.r), std::fabs(gg - want.g), std::fabs(gb - want.b) });
				++n;
				ok += d < 0.01 + 0.03 * std::max({ std::fabs(want.r), std::fabs(want.g), std::fabs(want.b) });
				worst = std::max(worst, d);
				warmer += gr > 0.03f && gr > gb + 0.01f;
			}
		}
		std::printf("  per channel, shader vs C++ mirror: %d of %d agree (worst %.4f); torch-red above blue on %d px\n", ok, n, worst, warmer);
		Check(n > 1000 && ok >= n * 98 / 100, "coloured lamps (key at level 0.6 + an extra) in a coloured room: every channel matches the C++ mirror on 98 % of pixels", ok, n);
		Check(warmer > 1000, "the torch's reflection is redder than blue against a bluish room", warmer, 1000);
	}

	// ---- 3e. the reflected path's occlusion: the bridge blocks some of the light on its way to the walls ---
	std::printf("[occlusion]\n");
	{
		auto litCount = [&](bool testOn) {
			ZoneGpuInput zo = zgm;
			zo.occlusion = testOn;
			DrawScene(g, cam);
			DrawPass(g, cam, zo);
			const auto img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
			int        n = 0, wallN = 0;
			for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
				if (ids[i] != kNone && base[i * 3 + 2] > 1e-4f && img[i * 3 + 2] / base[i * 3 + 2] - 1.0f > 0.02f) {
					++n;
					wallN += ids[i] == kWallSW_X || ids[i] == kWallSW_Y;
				}
			}
			return std::pair{ n, wallN };
		};
		const auto [openN, openWall] = litCount(false);
		const auto [occN, occWall] = litCount(true);
		std::printf("  lit pixels without / with the occlusion test: %d / %d (walls %d / %d)\n", openN, occN, openWall, occWall);
		Check(occN < openN && occN > openN / 2, "the occlusion test removes some light (the bridge's shadow on the walls), not most", occN, openN);
	}

	// ---- 3f. the sun: parallel light from high in the north-east, with a directional ambient ------------
	std::printf("[sun]\n");
	{
		for (const bool direct : { false, true }) {
			ZoneGpuInput zs = z;
			zs.sun = true;
			zs.light = { 0.45f, 0.4f, 0.8f };  // toward the sun (not unit: PackPass normalises it)
			zs.ambientPos = { 0.05f, 0.05f, 0.3f };
			zs.ambientNeg = { 0.05f, 0.1f, 0.02f };
			zs.debugLight = !direct;
			zs.debugDirect = direct;
			DrawScene(g, cam);
			DrawPass(g, cam, zs);
			const auto          img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
			const PassConstants pc = PackPass(zs, fc);
			const int           ch = direct ? 0 : 2;
			std::array<int, kSurfaces> surfN{}, surfLit{};
			int    n = 0, ok = 0;
			double worst = 0;
			for (int y = 1; y < H - 1; ++y) {
				for (int x = 1; x < W - 1; ++x) {
					const size_t i = static_cast<size_t>(y) * W + x;
					const auto   s = ids[i];
					if (s == kNone || s >= kSurfaces || base[i * 3 + ch] <= 1e-4f) {
						continue;
					}
					const float got = img[i * 3 + ch] / base[i * 3 + ch] - 1.0f;
					++surfN[s];
					surfLit[s] += got > 0.02f;
					bool edge = false;
					for (int dy = -1; dy <= 1 && !edge; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if (ids[i + dy * W + dx] != s) {
								edge = true;
								break;
							}
						}
					}
					const auto wp = world(x, y);
					if (edge || !wp) {
						continue;
					}
					const auto  r = CausticBoost(pc, *wp, faceN[s], 1.0f, zs.mask.get(), nullptr, &occ);
					const float want = direct ? r.add.r : r.add.b;
					++n;
					const double d = std::fabs(got - want);
					ok += d < 0.01 + 0.01 * want;
					worst = std::max(worst, d);
					if (d >= 0.01 + 0.01 * want && std::getenv("CL_DEBUG_PIX")) {
						std::printf("    mismatch x %d y %d id %u got %.3f want %.3f\n", x, y, s, got, want);
					}
				}
			}
			std::printf("  %s: %d of %d agree (worst %.4f)\n", direct ? "direct term" : "light mask", ok, n, worst);
			for (int s = 1; s < kSurfaces; ++s) {
				if (surfN[s]) {
					std::printf("    %-22s lit %5.1f %%\n", kSurfaceNames[s], 100.0 * surfLit[s] / surfN[s]);
				}
			}
			Check(n > 1000 && ok >= n * 99 / 100, direct ? "sun: direct term matches the C++ mirror on 99 % of pixels" : "sun: light mask matches the C++ mirror on 99 % of pixels", ok, n);
			if (!direct) {
				const double under = surfN[kBridgeUnder] ? static_cast<double>(surfLit[kBridgeUnder]) / surfN[kBridgeUnder] : 0.0;
				const double floorLit = surfN[kPondBottom] ? static_cast<double>(surfLit[kPondBottom]) / surfN[kPondBottom] : 0.0;
				Check(under > 0.3, "sun: the bridge underside gets the reflected sunlight", under, 0.3);
				Check(floorLit > 0.3, "sun: the pond floor gets the refracted sunlight", floorLit, 0.3);
			} else {
				const double under = surfN[kBridgeUnder] ? static_cast<double>(surfLit[kBridgeUnder]) / surfN[kBridgeUnder] : 1.0;
				Check(under < 0.01, "sun: no direct sunlight on the bridge underside (it faces down)", under, 0.01);
			}
		}
	}

	// ---- 3b. the direct-light term the share divides by (debug view 3), per pixel vs the C++ mirror ---
	std::printf("[direct light]\n");
	{
		ZoneGpuInput zd = zg;
		zd.debugDirect = true;
		DrawScene(g, cam);
		DrawPass(g, cam, zd);
		const auto          dimg = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		const PassConstants pcd = PackPass(zd, fc);
		int                 n = 0, ok = 0, lit = 0;
		double              worst = 0;
		for (int y = 1; y < H - 1; ++y) {
			for (int x = 1; x < W - 1; ++x) {
				const size_t i = static_cast<size_t>(y) * W + x;
				const auto   s = ids[i];
				if (s == kNone || s >= kSurfaces || base[i * 3] <= 1e-4f) {
					continue;
				}
				bool edge = false;
				for (int dy = -1; dy <= 1 && !edge; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (ids[i + dy * W + dx] != s) {
							edge = true;
							break;
						}
					}
				}
				const auto wp = world(x, y);
				if (edge || !wp) {
					continue;
				}
				const float got = dimg[i * 3] / base[i * 3] - 1.0f;
				const float want = CausticBoost(pcd, *wp, faceN[s], 1.0f, zd.mask.get(), nullptr, &occ).add.r;
				++n;
				ok += std::fabs(got - want) < 0.01;
				worst = std::max(worst, static_cast<double>(std::fabs(got - want)));
				if (std::fabs(got - want) >= 0.01 && std::getenv("CL_DEBUG_PIX")) {
					std::printf("    mismatch x %d y %d id %u got %.3f want %.3f world (%.1f, %.1f, %.1f)\n", x, y, s, got, want, (*wp)[0], (*wp)[1], (*wp)[2]);
				}
				lit += got > 0.02f;
			}
		}
		std::printf("  direct term: %d of %d agree (worst %.4f), %d lit\n", ok, n, worst, lit);
		Check(n > 1000 && ok >= n * 99 / 100 && lit > 1000, "direct-light term matches the C++ mirror on 99 % of pixels", ok, n);
	}

	// ---- 4. dumps for the Blender reference (tools/blender-caustic-ref.py): one lamp, strength 1 -----
	// boost = caustic / base - 1 (red channel) at the calm and rough agitation of the Blender ripples
	WriteRaw(out + "/ref_ids.u32", ids);
	WriteRaw(out + "/ref_depth.f32", depth);
	for (const auto& [name, agitation] : { std::pair{ "calm", 0.27f }, std::pair{ "rough", 0.68f } }) {
		ZoneGpuInput zr = z;
		zr.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));  // Cycles has real shadows: so does this dump
		zr.useLit = true;
		zr.strength = 1.0f;
		zr.ambient = 1.0f;  // the compare script re-applies the share with the ambient measured in Cycles: R = boost x (D + 1)
		zr.spread = ReflectionSpread(agitation);
		zr.weights = CrossfadeWeights(agitation);  // the pattern layers as in game (the Cycles ripples: 6.1 deg -> calm 0.44 / normal 0.56)
		DrawScene(g, cam);
		DrawPass(g, cam, zr);
		const auto         img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		std::vector<float> boost(static_cast<size_t>(W) * H, 0.0f);
		for (size_t i = 0; i < boost.size(); ++i) {
			boost[i] = base[i * 3] > 1e-4f ? img[i * 3] / base[i * 3] - 1.0f : 0.0f;
		}
		WriteRaw(out + "/ref_mod_" + name + ".f32", boost);
	}
	{
		ZoneGpuInput zd = z;
		zd.debugDirect = true;
		DrawScene(g, cam);
		DrawPass(g, cam, zd);
		const auto         img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		std::vector<float> direct(static_cast<size_t>(W) * H, 0.0f);
		for (size_t i = 0; i < direct.size(); ++i) {
			direct[i] = base[i * 3] > 1e-4f ? img[i * 3] / base[i * 3] - 1.0f : 0.0f;  // D (clamped at 1; one lamp of colour 0.77 stays below)
		}
		WriteRaw(out + "/ref_mod_direct.f32", direct);
	}
	{
		ZoneGpuInput zt = z;
		zt.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));
		zt.rect = zt.mask->bounds;
		zt.useLit = true;
		zt.tides = true;
		zt.tidesSize = 192.0f;  // MakeTides: 192 x 192 cells
		zt.tidesFine = 0.0f;
		zt.strength = 1.0f;
		zt.ambient = 1.0f;
		zt.spread = SpreadFromSlope(SlopeAngle(kSlopeStill));
		DrawScene(g, cam);
		DrawPass(g, cam, zt);
		const auto         img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		std::vector<float> boost(static_cast<size_t>(W) * H, 0.0f);
		for (size_t i = 0; i < boost.size(); ++i) {
			boost[i] = base[i * 3] > 1e-4f ? img[i * 3] / base[i * 3] - 1.0f : 0.0f;
		}
		WriteRaw(out + "/ref_mod_tides.f32", boost);
	}
	{
		// the sun (blender-caustic-ref.py ... sun): the same calm water, parallel light toward kRefSun
		const Vec3   toSun{ 0.45f, 0.4f, 0.8f };
		const float  sl = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
		ZoneGpuInput zs = z;
		zs.sun = true;
		zs.light = toSun;
		zs.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, { toSun[0] / sl * 1e5f, toSun[1] / sl * 1e5f, toSun[2] / sl * 1e5f }));
		zs.useLit = true;
		zs.strength = 1.0f;
		zs.ambient = 1.0f;
		zs.spread = ReflectionSpread(0.27f);
		DrawScene(g, cam);
		DrawPass(g, cam, zs);
		auto               img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		std::vector<float> v(static_cast<size_t>(W) * H, 0.0f);
		for (size_t i = 0; i < v.size(); ++i) {
			v[i] = base[i * 3] > 1e-4f ? img[i * 3] / base[i * 3] - 1.0f : 0.0f;
		}
		WriteRaw(out + "/ref_mod_sun.f32", v);
		zs.debugDirect = true;
		DrawScene(g, cam);
		DrawPass(g, cam, zs);
		img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
		for (size_t i = 0; i < v.size(); ++i) {
			v[i] = base[i * 3] > 1e-4f ? img[i * 3] / base[i * 3] - 1.0f : 0.0f;
		}
		WriteRaw(out + "/ref_mod_sun_direct.f32", v);
	}
	{
		// the lamp as vanilla (1 - (d / 1.5 r)^2) and as an inverse-square sphere light (Cycles' own falloff: a 0.25 m
		// lamp, the reference renders' radius), from the C++ mirror at float precision (the tiny inverse-square values
		// fall between half-float steps on the GPU) with the pattern at its mean: blender-caustic-compare.py ... mirror_*
		for (const bool isl : { false, true }) {
			ZoneGpuInput zm2 = z;
			zm2.mask = WithLitPlane(*z.mask, BridgeLit(*z.mask, z.light));
			zm2.useLit = true;
			zm2.strength = 1.0f;
			zm2.ambient = 1.0f;
			zm2.spread = ReflectionSpread(0.27f);
			if (isl) {
				zm2.keyFalloff = CsInverseSquare(6.0f, 0.2f, 0.25f, false);
				zm2.keyFalloff.radius = 20000.0f;  // no cut within the scene: a plain inverse square
				zm2.keyFalloff.fadeZone = 1.0f / 252.0f;
				// one scale on the lamp: the share is a ratio (reflected / (direct + ambient measured in lamp units)), so only
				// the compare script's "directly lit" threshold (D > 0.1) sees it
				zm2.lightColor = { zm2.lightColor.r * 250.0f, zm2.lightColor.g * 250.0f, zm2.lightColor.b * 250.0f };
			}
			const PassConstants pb = PackPass(zm2, fc);
			ZoneGpuInput        zd2 = zm2;
			zd2.debugDirect = true;
			const PassConstants pd = PackPass(zd2, fc);
			std::vector<float> boost(static_cast<size_t>(W) * H, 0.0f), direct(static_cast<size_t>(W) * H, 0.0f);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					const size_t i = static_cast<size_t>(y) * W + x;
					const auto   s = ids[i];
					if (s == kNone || s >= kSurfaces) {
						continue;
					}
					const auto wp = world(x, y);
					if (!wp) {
						continue;
					}
					boost[i] = CausticBoost(pb, *wp, faceN[s], kPatternMean, zm2.mask.get(), nullptr, &occ).add.r;
					direct[i] = CausticBoost(pd, *wp, faceN[s], kPatternMean, zm2.mask.get(), nullptr, &occ).add.r;
				}
			}
			const std::string name = isl ? "mirror_isl" : "mirror_vanilla";
			WriteRaw(out + "/ref_mod_" + name + ".f32", boost);
			WriteRaw(out + "/ref_mod_" + name + "_direct.f32", direct);
		}
	}
	{
		// two lights at once (blender-caustic-ref.py ... both): the lamp (inverse square, as mirror_isl) and the sun
		// (as ref_mod_sun), once with the lamp as the key and the sun an extra, once the other way round. The compare
		// script fits each against the same Cycles render: a key swap must not change the walls.
		// CL_BOTH_LEVEL: the sun's strength over the lamp's in these units (the compare script measures it from the
		// single-light renders: blender-caustic-compare.py ... both_level)
		const char*       lv = std::getenv("CL_BOTH_LEVEL");
		const float       sunRel = lv ? std::strtof(lv, nullptr) : 1.0f;
		const Vec3        toSun{ 0.45f, 0.4f, 0.8f };
		const float       sl = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
		const Vec3        sd{ toSun[0] / sl, toSun[1] / sl, toSun[2] / sl };
		constexpr float   kLampScale = 250.0f;  // as mirror_isl
		Falloff           lampFo = CsInverseSquare(6.0f, 0.2f, 0.25f, false);
		lampFo.radius = 20000.0f;
		lampFo.fadeZone = 1.0f / 252.0f;
		const auto        lampLit = BridgeLit(*z.mask, z.light);
		const auto        sunLit = BridgeLit(*z.mask, { sd[0] * 1e5f, sd[1] * 1e5f, sd[2] * 1e5f });
		const RGB         lampCol{ z.lightColor.r * kLampScale, z.lightColor.g * kLampScale, z.lightColor.b * kLampScale };
		for (const bool sunKey : { false, true }) {
			ZoneGpuInput zb = z;
			zb.strength = 1.0f;
			zb.ambient = 1.0f;  // in the key's own units: the compare script reads R = dump x (D + 1)
			zb.spread = ReflectionSpread(0.27f);
			zb.level = 1.0f;
			zb.extra = {};
			zb.useLit = true;
			zb.useLitExtra = { true, false };
			if (!sunKey) {
				zb.mask = WithLitPlanes(*z.mask, { &lampLit, &sunLit, nullptr });
				zb.keyFalloff = lampFo;
				zb.lightColor = lampCol;
				zb.extra[0].pos = sd;
				zb.extra[0].sun = true;
				zb.extra[0].color = z.lightColor;
				zb.extra[0].level = sunRel;  // in the lamp's units (lampCol carries the 250)
				zb.extra[0].ref = 1;
			} else {
				zb.mask = WithLitPlanes(*z.mask, { &sunLit, &lampLit, nullptr });
				zb.sun = true;
				zb.light = sd;
				zb.lightColor = z.lightColor;
				zb.extra[0].pos = z.light;
				zb.extra[0].radius = z.lightRadius;
				zb.extra[0].falloff = lampFo;
				zb.extra[0].color = z.lightColor;
				zb.extra[0].level = kLampScale / std::max(sunRel, 1e-6f);  // the lamp in the sun's units
				zb.extra[0].ref = 2;
			}
			const PassConstants pb = PackPass(zb, fc);
			ZoneGpuInput        zd = zb;
			zd.debugDirect = true;
			const PassConstants pd = PackPass(zd, fc);
			std::vector<float>  boost(static_cast<size_t>(W) * H, 0.0f), direct(static_cast<size_t>(W) * H, 0.0f);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					const size_t i = static_cast<size_t>(y) * W + x;
					const auto   s = ids[i];
					if (s == kNone || s >= kSurfaces) {
						continue;
					}
					const auto wp = world(x, y);
					if (!wp) {
						continue;
					}
					boost[i] = CausticBoost(pb, *wp, faceN[s], kPatternMean, zb.mask.get(), nullptr, &occ).add.r;
					direct[i] = CausticBoost(pd, *wp, faceN[s], kPatternMean, zb.mask.get(), nullptr, &occ).add.r;
				}
			}
			const std::string name = sunKey ? "mirror_both_sunkey" : "mirror_both_lampkey";
			WriteRaw(out + "/ref_mod_" + name + ".f32", boost);
			WriteRaw(out + "/ref_mod_" + name + "_direct.f32", direct);
		}
		std::printf("  two-light dumps (lamp key / sun key) at the sun's level %.4f in lamp units\n", sunRel);
	}
	std::printf("[reference dumps] ref_ids.u32, ref_mod_calm.f32 / ref_mod_rough.f32 / ref_mod_tides.f32 (ambient 1), ref_mod_direct.f32 (%d x %d)\n", W, H);

	// ---- 6. the Surface Tides bind watch (DrawPass.cpp InstallTidesWatch): the vtable slots it patches on the real
	// d3d11 context are PSSetShaderResources (8) and PSSetConstantBuffers (16); a wrong slot would crash the game
	std::printf("[bind watch]\n");
	{
		static int                       srvCalls = 0, cbCalls = 0;
		static ID3D11ShaderResourceView* seenSrv = nullptr;
		static ID3D11Buffer*             seenCb = nullptr;
		using SetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
		using SetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
		static SetSrvFn origSrv = nullptr;
		static SetCbFn  origCb = nullptr;
		struct Hooks
		{
			static void STDMETHODCALLTYPE Srv(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11ShaderResourceView* const* v)
			{
				origSrv(c, st, n, v);
				if (v && st <= 96 && st + n > 96) {
					++srvCalls;
					seenSrv = v[96 - st];
				}
			}
			static void STDMETHODCALLTYPE Cb(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* v)
			{
				origCb(c, st, n, v);
				if (v && st <= 11 && st + n > 11) {
					++cbCalls;
					seenCb = v[11 - st];
				}
			}
		};
		void**     vt = *reinterpret_cast<void***>(g.ctx);
		DWORD      old = 0, tmp = 0;
		const bool prot = VirtualProtect(&vt[8], sizeof(void*) * 9, PAGE_EXECUTE_READWRITE, &old) != 0;
		Check(prot, "the context vtable can be made writable");
		if (prot) {
			origSrv = reinterpret_cast<SetSrvFn>(vt[8]);
			origCb = reinterpret_cast<SetCbFn>(vt[16]);
			vt[8] = reinterpret_cast<void*>(&Hooks::Srv);
			vt[16] = reinterpret_cast<void*>(&Hooks::Cb);
			// a field-like texture and a buffer, bound the way Surface Tides binds them
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = 16;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			ID3D11Texture2D*          tex = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			g.dev->CreateTexture2D(&td, nullptr, &tex);
			if (tex) {
				g.dev->CreateShaderResourceView(tex, nullptr, &srv);
			}
			D3D11_BUFFER_DESC bd{ 96, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
			ID3D11Buffer*     cb = nullptr;
			g.dev->CreateBuffer(&bd, nullptr, &cb);
			g.ctx->PSSetShaderResources(96, 1, &srv);
			g.ctx->PSSetConstantBuffers(11, 1, &cb);
			g.ctx->PSSetShaderResources(0, 1, &srv);  // another slot: not the field
			ID3D11ShaderResourceView* back = nullptr;
			g.ctx->PSGetShaderResources(96, 1, &back);
			Check(srvCalls == 1 && seenSrv == srv && srv, "the watch sees PSSetShaderResources at slot 96 (vtable 8)", srvCalls, 1);
			Check(cbCalls == 1 && seenCb == cb && cb, "the watch sees PSSetConstantBuffers at slot 11 (vtable 16)", cbCalls, 1);
			Check(back == srv, "the watched call still binds (passed on unchanged)");
			if (back) {
				back->Release();
			}
			ID3D11ShaderResourceView* none = nullptr;
			ID3D11Buffer*             noCb = nullptr;
			g.ctx->PSSetShaderResources(96, 1, &none);
			g.ctx->PSSetConstantBuffers(11, 1, &noCb);
			g.ctx->PSSetShaderResources(0, 1, &none);
			vt[8] = reinterpret_cast<void*>(origSrv);
			vt[16] = reinterpret_cast<void*>(origCb);
			VirtualProtect(&vt[8], sizeof(void*) * 9, old, &tmp);
			if (srv) {
				srv->Release();
			}
			if (tex) {
				tex->Release();
			}
			if (cb) {
				cb->Release();
			}
		}
	}
	// ---- 6b. the vertex / domain stage slots the watch also patches: VSSetShaderResources 25, VSSetConstantBuffers 7,
	// DSSetShaderResources 63, DSSetConstantBuffers 66
	std::printf("[bind watch, VS / DS]\n");
	{
		using SetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
		using SetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
		static SetSrvFn origS[2] = {};
		static SetCbFn  origC[2] = {};
		static int      srvHits[2] = {}, cbHits[2] = {};
		struct H
		{
			static void STDMETHODCALLTYPE S0(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11ShaderResourceView* const* v) { origS[0](c, st, n, v); srvHits[0] += (st <= 96 && st + n > 96); }
			static void STDMETHODCALLTYPE S1(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11ShaderResourceView* const* v) { origS[1](c, st, n, v); srvHits[1] += (st <= 96 && st + n > 96); }
			static void STDMETHODCALLTYPE C0(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* v) { origC[0](c, st, n, v); cbHits[0] += (st <= 11 && st + n > 11); }
			static void STDMETHODCALLTYPE C1(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* v) { origC[1](c, st, n, v); cbHits[1] += (st <= 11 && st + n > 11); }
		};
		const int  srvSlot[2] = { 25, 63 }, cbSlot[2] = { 7, 66 };
		void**     vt = *reinterpret_cast<void***>(g.ctx);
		DWORD      old = 0, tmp = 0;
		const bool prot = VirtualProtect(&vt[7], sizeof(void*) * 60, PAGE_EXECUTE_READWRITE, &old) != 0;
		Check(prot, "the context vtable (slots 7..66) can be made writable");
		if (prot) {
			for (int s = 0; s < 2; ++s) {
				origS[s] = reinterpret_cast<SetSrvFn>(vt[srvSlot[s]]);
				origC[s] = reinterpret_cast<SetCbFn>(vt[cbSlot[s]]);
			}
			vt[25] = reinterpret_cast<void*>(&H::S0);
			vt[63] = reinterpret_cast<void*>(&H::S1);
			vt[7] = reinterpret_cast<void*>(&H::C0);
			vt[66] = reinterpret_cast<void*>(&H::C1);
			ID3D11ShaderResourceView* none = nullptr;
			ID3D11Buffer*             noCb = nullptr;
			g.ctx->VSSetShaderResources(96, 1, &none);
			g.ctx->DSSetShaderResources(96, 1, &none);
			g.ctx->VSSetConstantBuffers(11, 1, &noCb);
			g.ctx->DSSetConstantBuffers(11, 1, &noCb);
			for (int s = 0; s < 2; ++s) {
				vt[srvSlot[s]] = reinterpret_cast<void*>(origS[s]);
				vt[cbSlot[s]] = reinterpret_cast<void*>(origC[s]);
			}
			VirtualProtect(&vt[7], sizeof(void*) * 60, old, &tmp);
			Check(srvHits[0] == 1 && cbHits[0] == 1, "vtable 25 / 7 are VSSetShaderResources / VSSetConstantBuffers", srvHits[0] + cbHits[0], 2);
			Check(srvHits[1] == 1 && cbHits[1] == 1, "vtable 63 / 66 are DSSetShaderResources / DSSetConstantBuffers", srvHits[1] + cbHits[1], 2);
		}
	}
	// ---- 7. a Community Shaders inverse-square lamp (Light Placer 'InverseSquare'): direct term and light mask vs the mirror
	std::printf("[inverse-square lamp]\n");
	{
		for (const bool direct : { true, false }) {
			ZoneGpuInput zi = z;
			zi.keyFalloff = CsInverseSquare(6.0f, 0.2f, 3.4f, false);
			zi.keyFalloff.radius = 1500.0f;  // reach the whole scene (the attenuation's shape, not its cut, is compared)
			zi.keyFalloff.fadeZone = 1.0f / 252.0f;
			zi.debugDirect = direct;
			zi.debugLight = !direct;
			DrawScene(g, cam);
			DrawPass(g, cam, zi);
			const auto          img = Read<float>(g, g.color, DXGI_FORMAT_UNKNOWN, 3, true);
			const PassConstants pc = PackPass(zi, fc);
			const int           ch = direct ? 0 : 2;
			int                 n = 0, ok = 0, lit = 0;
			for (int y = 1; y < H - 1; ++y) {
				for (int x = 1; x < W - 1; ++x) {
					const size_t i = static_cast<size_t>(y) * W + x;
					const auto   s = ids[i];
					if (s == kNone || s >= kSurfaces || base[i * 3 + ch] <= 1e-4f) {
						continue;
					}
					bool edge = false;
					for (int dy = -1; dy <= 1 && !edge; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if (ids[i + dy * W + dx] != s) {
								edge = true;
								break;
							}
						}
					}
					const auto wp = world(x, y);
					if (edge || !wp) {
						continue;
					}
					const float got = img[i * 3 + ch] / base[i * 3 + ch] - 1.0f;
					const auto  r = CausticBoost(pc, *wp, faceN[s], 1.0f, zi.mask.get(), nullptr, &occ);
					const float want = direct ? r.add.r : r.add.b;
					++n;
					ok += std::fabs(got - want) < 0.01 + 0.01 * want;
					lit += got > 0.002f;
				}
			}
			std::printf("  %s: %d of %d agree, %d lit\n", direct ? "direct term" : "light mask", ok, n, lit);
			Check(n > 1000 && ok >= n * 99 / 100 && lit > 1000, direct ? "inverse-square lamp: direct term matches the mirror on 99 % of pixels" :
			                                                             "inverse-square lamp: light mask matches the mirror on 99 % of pixels", ok, n);
		}
	}
	std::printf("\n%d checks, %d failed -> %s\n", g_checks, g_failures, g_failures ? "FAIL" : "ALL PASS");
	return g_failures ? 1 : 0;
}
