// Samples one atlas cell the way the game will: quads side by side, UVs inset to the cell's content,
// bilinear filtering on the whole atlas. The step across a quad border must look like any other step.
'use strict';
const fs = require('fs'), zlib = require('zlib');
const [file, G = 4, COLS = 8] = process.argv.slice(2).map((a, i) => i ? +a : a);

function readPng(f) {   // only what gen.cjs writes: 8-bit RGBA, filter 0
  const b = fs.readFileSync(f); let p = 8, w, h; const idat = [];
  while (p < b.length) {
    const len = b.readUInt32BE(p), type = b.toString('latin1', p + 4, p + 8), d = b.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); if (d[9] !== 6) throw new Error('RGBA only'); }
    if (type === 'IDAT') idat.push(d);
    p += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat)), a = new Float64Array(w * h);
  for (let y = 0; y < h; y++) { if (raw[y * (w * 4 + 1)] !== 0) throw new Error('filter'); for (let x = 0; x < w; x++) a[y * w + x] = raw[y * (w * 4 + 1) + 1 + x * 4 + 3]; }
  return { w, h, a };
}
const img = readPng(file), A = img.w, CELL = A / COLS, S = CELL - 2 * G;
const tex = (x, y) => img.a[Math.min(A - 1, Math.max(0, y)) * A + Math.min(A - 1, Math.max(0, x))];
function bilinear(u, v) {   // u, v in atlas texels, texel centres at .5
  const x = u - 0.5, y = v - 0.5, x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
  return (1 - fx) * (1 - fy) * tex(x0, y0) + fx * (1 - fy) * tex(x0 + 1, y0) + (1 - fx) * fy * tex(x0, y0 + 1) + fx * fy * tex(x0 + 1, y0 + 1);
}
// 1. exact: every gutter texel equals the content texel it wraps around to (so bilinear at a quad border
//    blends the frame with itself, never with the neighbouring frame)
let bad = 0;
for (let cell = 0; cell < COLS * COLS; cell++) {
  const ox = (cell % COLS) * CELL + G, oy = Math.floor(cell / COLS) * CELL + G;
  for (let y = -G; y < S + G; y++) for (let x = -G; x < S + G; x++) {
    const wx = ((x % S) + S) % S, wy = ((y % S) + S) % S;
    if (tex(ox + x, oy + y) !== tex(ox + wx, oy + wy)) bad++;
  }
}
console.log(`gutter mismatches: ${bad}`);
// 2. statistical: across all 64 frames, the step at the quad border vs. the steps in the columns next to it
let seamSum = 0, nearSum = 0, nearN = 0, rows = 0;
for (let cell = 0; cell < COLS * COLS; cell++) {
  const ox = (cell % COLS) * CELL + G, oy = Math.floor(cell / COLS) * CELL + G;
  for (let y = 0; y < S; y++) {
    const at = q => bilinear(ox + ((q % S) + S) % S + 0.5, oy + y + 0.5);   // two quads side by side, q = screen column
    seamSum += Math.abs(at(S) - at(S - 1)); rows++;
    for (const d of [-4, -3, -2, 2, 3, 4]) { nearSum += Math.abs(at(S + d) - at(S + d - 1)); nearN++; }
  }
}
const ratio = (seamSum / rows) / (nearSum / nearN);
console.log(`border step / neighbouring steps = ${ratio.toFixed(3)}`);
const ok = bad === 0 && ratio < 1.15;
console.log(ok ? 'OK: quad borders look like ordinary steps' : 'SEAM: quad borders stand out');
process.exitCode = ok ? 0 : 1;
