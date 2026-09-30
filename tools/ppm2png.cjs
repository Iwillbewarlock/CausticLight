// Converts binary PPM (P6) files to PNG: node tools/ppm2png.cjs a.ppm [b.ppm ...] -> a.png ...
'use strict';
const fs = require('fs'), zlib = require('zlib');
const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = b => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t, 'latin1'), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
for (const f of process.argv.slice(2)) {
  const b = fs.readFileSync(f);
  const m = b.toString('latin1', 0, 64).match(/^P6\s+(\d+)\s+(\d+)\s+(\d+)\s/);
  if (!m) throw new Error(`${f}: not a P6 PPM`);
  const w = +m[1], h = +m[2], px = b.subarray(m[0].length);
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) px.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const out = f.replace(/\.ppm$/i, '.png');
  fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  console.log(out);
}
