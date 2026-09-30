// Splits a gen.cjs atlas (RGB = the walls' reflection screens, alpha = the floor's refraction pattern) into two
// files, so BC7 spends each block's bits on one pattern: <floor.png> keeps the alpha under white RGB (the atlas
// exactly as it was before the reflection channels), <refl.png> keeps the RGB under an opaque alpha.
//   node tools/split-atlas.cjs <atlas.png> <floor.png> <refl.png>
'use strict';
const fs = require('fs'), zlib = require('zlib');
const [src, floorOut, reflOut] = process.argv.slice(2);

function readPng(f) {   // only what gen.cjs writes: 8-bit RGBA, filter 0
  const b = fs.readFileSync(f); let p = 8, w, h; const idat = [];
  while (p < b.length) {
    const len = b.readUInt32BE(p), type = b.toString('latin1', p + 4, p + 8), d = b.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); if (d[9] !== 6) throw new Error('RGBA only'); }
    if (type === 'IDAT') idat.push(d);
    p += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat)), px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    if (raw[y * (w * 4 + 1)] !== 0) throw new Error('filter');
    px.set(raw.subarray(y * (w * 4 + 1) + 1, (y + 1) * (w * 4 + 1)), y * w * 4);
  }
  return { w, h, px };
}
function png(file, w, h, px) {
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = b => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]), c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const rw = w * 4 + 1, raw = Buffer.alloc(rw * h);
  for (let y = 0; y < h; y++) { raw[y * rw] = 0; Buffer.from(px.buffer, px.byteOffset + y * w * 4, w * 4).copy(raw, y * rw + 1); }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]));
}

const { w, h, px } = readPng(src);
const floor = new Uint8Array(px.length).fill(255), refl = new Uint8Array(px);
for (let i = 3; i < px.length; i += 4) { floor[i] = px[i]; refl[i] = 255; }
png(floorOut, w, h, floor);
png(reflOut, w, h, refl);
console.log(`${src} -> ${floorOut} (alpha), ${reflOut} (RGB)`);
