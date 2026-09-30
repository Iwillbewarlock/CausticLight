// Tileable, seamlessly looping water-caustic frames.
// Wave recipe adapted from CAUSTIC//LITE (https://github.com/ScottieFox/caustic-volume, MIT, (c) 2026 Scottie):
// ~24 waves from 1.2 m down to 12 cm, golden-angle spread, amplitude chosen so each wave bends light about equally.
// Changes: every wave vector sits on the tile's lattice (so the pattern tiles in space) and every angular speed
// is a whole number of turns per loop (so the last frame runs straight into the first).
'use strict';
const fs = require('fs'), path = require('path'), zlib = require('zlib');

const args = Object.fromEntries(process.argv.slice(2).map(a => a.replace(/^--/, '').split('=')));
const L = +(args.L ?? 1.2);          // tile size (m)
const S = +(args.S ?? 512);          // output frame size (px)
const P = +(args.P ?? 3 * S);        // photon grid per side
const F = +(args.F ?? 64);           // frames per loop
const JMAX = +(args.J ?? 6);         // fastest wave: turns per loop
const D = +(args.D ?? 0.8);          // surface-to-screen distance (m)
const AMP = +(args.A ?? 1);          // wave height scale
const TURB = +(args.T ?? 1);         // extra scale for the short waves (under 30 cm), as the lite's Turbulence
const OUT = args.out ?? 'out';
const SPREAD = +(args.spread ?? 360) * Math.PI / 180;   // wave directions within this fan (360 = every direction)
const DIR0 = +(args.dir ?? 0) * Math.PI / 180;           // centre of the fan
const STAND = +(args.stand ?? 0);                        // 0..1: a counter-running copy of each wave, so the net shimmers in place
// reflection: the same sea as a mirror, light straight down reflected up onto a screen this far above (m), one per
// RGB channel (atlas mode). Stored linear (intensity / the channel's p99.9, mean printed), unlike the refraction
// alpha: the shader reads them as light (mean 1) and picks the channel by the wall's distance from the water.
const MIRROR = args.mirror ? String(args.mirror).split(',').map(Number) : [];
const IOR = 1.333;

// ---------------------------------------------------------------- the waves, snapped to the tile lattice
// the spectrum: NW waves from LMAX (m) down to LMAX x LRATIO; a wave shorter than KNEE bends light as much as any
// other (its slope amplitude the same), a longer one less ((KNEE / len)^2): the lite's recipe is KNEE 0.55.
// Two seas: the refraction one (the floor, alpha) from the plain arguments, the reflection one (the walls, RGB)
// from the same names with an 'm' in front (--mA, --mT, --mnw, --mlmax, --mlratio, --mknee, --mstand, --mJ), each
// defaulting to the plain one.
function makeSea(pre) {
  const g = (k, d) => +(args[pre + k] ?? args[k] ?? d);
  const LMAX = g('lmax', 1.2), LRATIO = g('lratio', 0.1), KNEE = g('knee', 0.55), NW = g('nw', 24), J = g('J', 6);
  const WAVES = [];
  let s = 5;
  const rnd = () => (s = (s * 16807) % 2147483647) / 2147483647;
  const seen = new Set();
  for (let i = 0; i < NW; i++) {
    const len0 = LMAX * LRATIO ** (i / Math.max(NW - 1, 1)), k0 = 2 * Math.PI / len0, dirRaw = i * 2.39996 + (rnd() - 0.5) * 0.9;
    const wrapped = ((dirRaw + Math.PI) % (2 * Math.PI) + 2 * Math.PI) % (2 * Math.PI) - Math.PI, dir = DIR0 + wrapped * SPREAD / (2 * Math.PI);
    const ph = rnd() * 6.283, ar = 0.6 + 0.8 * rnd(), sw = rnd() * 6.283;
    let n = Math.round(L * k0 * Math.cos(dir) / (2 * Math.PI)), m = Math.round(L * k0 * Math.sin(dir) / (2 * Math.PI));
    if (n === 0 && m === 0) n = 1;
    const key = (n < 0 || (n === 0 && m < 0)) ? `${-n},${-m}` : `${n},${m}`;   // (n,m) and (-n,-m) are the same wave
    if (seen.has(key)) continue;
    seen.add(key);
    const kx = 2 * Math.PI * n / L, kz = 2 * Math.PI * m / L, k = Math.hypot(kx, kz), len = 2 * Math.PI / k;
    const a = 0.55 * Math.min(1, (KNEE / len) ** 2) / (k * k) * ar;
    WAVES.push({ n, m, kx, kz, k, len, a, ph, sw, w: Math.sqrt(9.81 * k + 0.074 * k ** 3) });
  }
  const wMax = Math.max(...WAVES.map(W => W.w));
  WAVES.forEach((W, i) => { W.j = Math.max(1, Math.round(W.w / wMax * J)); W.q = 1 + (i % 2); });   // turns per loop
  return { WAVES, AMP: g('A', 1), TURB: g('T', 1), STAND: g('stand', 0) };
}
const SEA = makeSea(''), MSEA = makeSea('m'), WAVES = SEA.WAVES;

// height and slope of the sea at loop phase tau in [0,1)
function frameTables(tau, rep = 1, sea = SEA) {   // evaluated at the real position over rep x rep tiles (no wrapping assumed)
  const Q = P * rep;
  const { AMP, TURB, STAND } = sea;
  return sea.WAVES.map(W => {
    const a = AMP * (W.len < 0.3 ? TURB : 1) * W.a * (1 + 0.35 * Math.sin(2 * Math.PI * W.q * tau + W.sw));   // each wave's height swings, W.q times per loop
    const c = W.ph - 2 * Math.PI * W.j * tau, c2 = W.ph * 1.618 + 1.3 + 2 * Math.PI * W.j * tau;
    const cx = new Float64Array(Q), sx = new Float64Array(Q), cz = new Float64Array(Q), sz = new Float64Array(Q), cz2 = new Float64Array(Q), sz2 = new Float64Array(Q);
    for (let u = 0; u < Q; u++) {
      const px = 2 * Math.PI * W.n * u / P, pz = 2 * Math.PI * W.m * u / P + c;
      cx[u] = Math.cos(px); sx[u] = Math.sin(px); cz[u] = Math.cos(pz); sz[u] = Math.sin(pz);
      const pz2 = 2 * Math.PI * W.m * u / P + c2; cz2[u] = Math.cos(pz2); sz2[u] = Math.sin(pz2);
    }
    return { a, a2: a * STAND, kx: W.kx, kz: W.kz, cx, sx, cz, sz, cz2, sz2 };
  });
}

// light straight down, refracted at the surface, landing on a screen D below (or, mirrorL > 0, reflected up onto a
// screen mirrorL above); wrapped into the tile
function renderFrame(tau, rep = 1, mirrorL = 0, sea = SEA) {
  const T = frameTables(tau, rep, sea), N = S * rep, Q = P * rep, img = new Float64Array(N * N), cell = L / P, pxPerM = S / L;
  for (let v = 0; v < Q; v++) for (let u = 0; u < Q; u++) {
    let h = 0, gx = 0, gz = 0;
    for (let i = 0; i < T.length; i++) {
      const t = T[i], s = t.sx[u] * t.cz[v] + t.cx[u] * t.sz[v], c = t.cx[u] * t.cz[v] - t.sx[u] * t.sz[v];
      h += t.a * s; const ac = t.a * c; gx += ac * t.kx; gz += ac * t.kz;
      if (t.a2) { const s2 = t.sx[u] * t.cz2[v] + t.cx[u] * t.sz2[v], c2 = t.cx[u] * t.cz2[v] - t.sx[u] * t.sz2[v];
        h += t.a2 * s2; const ac2 = t.a2 * c2; gx += ac2 * t.kx; gz += ac2 * t.kz; }
    }
    const inv = 1 / Math.hypot(gx, 1, gz), nx = -gx * inv, ny = inv, nz = -gz * inv;
    let dx, dy, dz, dist;
    if (mirrorL > 0) {
      // reflect (0,-1,0) off the normal: (0,-1,0) + 2 ny n
      dx = 2 * ny * nx; dy = -1 + 2 * ny * ny; dz = 2 * ny * nz;
      dist = (mirrorL - h) / Math.max(dy, 1e-6);
    } else {
      // refract (0,-1,0) through the normal (-gx, 1, -gz)
      const eta = 1 / IOR, ci = ny, k = 1 - eta * eta * (1 - ci * ci);
      const f = eta * ci - Math.sqrt(k);
      dx = f * nx; dy = -eta + f * ny; dz = f * nz;
      dist = (D + h) / -dy;
    }
    const X = ((u + 0.5) * cell + dx * dist) * pxPerM - 0.5, Z = ((v + 0.5) * cell + dz * dist) * pxPerM - 0.5;
    const x0 = Math.floor(X), z0 = Math.floor(Z), fx = X - x0, fz = Z - z0;
    const xa = ((x0 % N) + N) % N, xb = (xa + 1) % N, za = ((z0 % N) + N) % N, zb = (za + 1) % N;
    img[za * N + xa] += (1 - fx) * (1 - fz); img[za * N + xb] += fx * (1 - fz);
    img[zb * N + xa] += (1 - fx) * fz;       img[zb * N + xb] += fx * fz;
  }
  const norm = N * N / (P * P * rep * rep);
  for (let i = 0; i < img.length; i++) img[i] *= norm;   // mean 1 = unfocused light
  return blur(img, N);
}
// the sea's RMS slope (deg): each wave a k A, its height swinging by +-35 % (mean square 1 + 0.35^2 / 2), plus the
// counter-running copy
function rmsSlopeDeg(sea) {
  const { AMP, TURB, STAND } = sea;
  let ms = 0;
  for (const W of sea.WAVES) {
    const a = AMP * (W.len < 0.3 ? TURB : 1) * W.a;
    ms += 0.5 * (a * W.k) ** 2 * (1 + 0.35 * 0.35 / 2) * (1 + STAND * STAND);
  }
  return Math.atan(Math.sqrt(ms)) * 180 / Math.PI;
}
function cvOf(all, mean) { let v = 0; for (const x of all) v += (x - mean) ** 2; return Math.sqrt(v / all.length) / mean; }
function blur(img, N) {   // 1-2-1 each way, wrapping
  const t = new Float64Array(N * N), o = new Float64Array(N * N);
  for (let z = 0; z < N; z++) for (let x = 0; x < N; x++)
    t[z * N + x] = 0.25 * img[z * N + (x + N - 1) % N] + 0.5 * img[z * N + x] + 0.25 * img[z * N + (x + 1) % N];
  for (let z = 0; z < N; z++) for (let x = 0; x < N; x++)
    o[z * N + x] = 0.25 * t[((z + N - 1) % N) * N + x] + 0.5 * t[z * N + x] + 0.25 * t[((z + 1) % N) * N + x];
  return o;
}
// light above the unfocused level becomes brightness; the flat level itself is black, so it adds as pure highlight
const tone = I => Math.round(255 * Math.pow(1 - Math.exp(-0.6 * Math.max(0, I - 0.85)), 0.9));

function png(file, w, h, px, ch = 1) {   // ch 1 = grey, 4 = RGBA
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = b => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]), c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const rw = w * ch + 1, raw = Buffer.alloc(rw * h);
  for (let y = 0; y < h; y++) { raw[y * rw] = 0; for (let x = 0; x < w * ch; x++) raw[y * rw + 1 + x] = px[y * w * ch + x]; }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = ch === 4 ? 6 : 0;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]));
}

// ---------------------------------------------------------------- checks, then frames
const mode = args.mode ?? 'frames';
if (mode === 'check') {
  // space: render a 2x2 block of tiles as one big wrapped tile; all four quadrants must match
  const big = renderFrame(0.3, 2), N = 2 * S; let dq = 0, mx = 0;
  for (let z = 0; z < S; z++) for (let x = 0; x < S; x++) {
    const a = big[z * N + x];
    for (const [ox, oz] of [[S, 0], [0, S], [S, S]]) dq = Math.max(dq, Math.abs(a - big[(z + oz) * N + x + ox]));
    mx = Math.max(mx, a);
  }
  // time: the frame one full loop later must equal the first
  const f0 = renderFrame(0), f1 = renderFrame(1); let dt = 0;
  for (let i = 0; i < f0.length; i++) dt = Math.max(dt, Math.abs(f0[i] - f1[i]));
  // seam: the step across the tile edge vs. the average step between neighbouring columns
  let edge = 0, inner = 0;
  for (let z = 0; z < S; z++) { edge += Math.abs(f0[z * S] - f0[z * S + S - 1]); for (let x = 1; x < S; x++) inner += Math.abs(f0[z * S + x] - f0[z * S + x - 1]); }
  console.log(JSON.stringify({ waves: WAVES.length, lattice: WAVES.map(W => [W.n, W.m, W.j]), peak: +mx.toFixed(2),
    quadrantMaxDiff: dq, loopMaxDiff: dt, seamStep: +(edge / S).toFixed(4), innerStep: +(inner / (S * (S - 1))).toFixed(4) }));
} else if (mode === 'atlas') {
  // 8 x 8 cells of CELL px; each frame is rendered at S = CELL - 2*G and surrounded by a G px gutter copied
  // from its own opposite edges, so bilinear filtering at a quad's edge blends with the same frame, not the next one.
  // RGB is white and the pattern is in alpha, like the vanilla MagicCausticTile.
  const G = +(args.G ?? 4), CELL = S + 2 * G, COLS = Math.round(Math.sqrt(F)), A = COLS * CELL;
  if (COLS * COLS !== F) throw new Error('F must be a square number');
  const px = new Uint8Array(A * A * 4).fill(255);
  const mir = MIRROR.map(() => []);  // per channel, per frame: the reflected light (mean 1)
  for (let f = 0; f < F; f++) {
    const img = renderFrame(f / F), cx = (f % COLS) * CELL, cy = Math.floor(f / COLS) * CELL;
    for (let y = 0; y < CELL; y++) for (let x = 0; x < CELL; x++) {
      const sx = ((x - G) % S + S) % S, sy = ((y - G) % S + S) % S;
      px[((cy + y) * A + cx + x) * 4 + 3] = tone(img[sy * S + sx]);
    }
    MIRROR.forEach((Lm, c) => mir[c].push(Float32Array.from(renderFrame(f / F, 1, Lm, MSEA))));
    process.stdout.write(`\r${OUT}: ${f + 1}/${F}`);
  }
  const report = { rmsSlopeDeg: +rmsSlopeDeg(SEA).toFixed(2), mirrorRmsSlopeDeg: +rmsSlopeDeg(MSEA).toFixed(2), alphaMean: 0, mirror: [] };
  MIRROR.forEach((Lm, c) => {
    const all = Float32Array.from(mir[c].flatMap(a => Array.from(a))).sort();
    const peak = all[Math.floor(0.999 * (all.length - 1))];  // the channel's p99.9: brighter specks clip
    let sum = 0, mean = 0, lit = 0;
    for (const v of all) { mean += v; lit += v > 1.03 ? 1 : 0; }
    for (let f = 0; f < F; f++) {
      const img = mir[c][f], cx = (f % COLS) * CELL, cy = Math.floor(f / COLS) * CELL;
      for (let y = 0; y < CELL; y++) for (let x = 0; x < CELL; x++) {
        const sx = ((x - G) % S + S) % S, sy = ((y - G) % S + S) % S;
        const v = Math.round(255 * Math.min(1, img[sy * S + sx] / peak));
        px[((cy + y) * A + cx + x) * 4 + c] = v;
        if (x >= G && x < G + S && y >= G && y < G + S) sum += v;
      }
    }
    report.mirror.push({ L: Lm, peak: +peak.toFixed(3), encodedMean: +(sum / (F * S * S) / 255).toFixed(5),
      cv: +cvOf(all, mean / all.length).toFixed(3), litShare: +(lit / all.length).toFixed(3) });
  });
  { let s = 0, n = 0; for (let f = 0; f < F; f++) { const cx = (f % COLS) * CELL + G, cy = Math.floor(f / COLS) * CELL + G;
      for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) { s += px[((cy + y) * A + cx + x) * 4 + 3]; n++; } }
    report.alphaMean = +(s / n / 255).toFixed(4); }
  png(OUT, A, A, px, 4);
  console.log(`
${OUT}: ${A}x${A}, cell ${CELL} (content ${S}, gutter ${G}) ${JSON.stringify(report)}`);
} else {
  fs.mkdirSync(OUT, { recursive: true });
  const stats = [];
  for (let f = 0; f < F; f++) {
    const img = renderFrame(f / F), px = new Uint8Array(S * S);
    let sat = 0; for (let i = 0; i < img.length; i++) { px[i] = tone(img[i]); if (px[i] >= 250) sat++; }
    png(path.join(OUT, `f${String(f).padStart(3, '0')}.png`), S, S, px);
    stats.push(sat / img.length);
    process.stdout.write(`\r${OUT}: ${f + 1}/${F}`);
  }
  console.log(`\nsaturated share: max ${(100 * Math.max(...stats)).toFixed(2)}%`);
}
