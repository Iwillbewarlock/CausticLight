// Lists EditorIDs (and FormIDs) of records whose EDID matches a pattern, for picking console targets.
//   node tools/esm-find.cjs <plugin.esm> <TYPE[,TYPE]> <regex>
'use strict';
const fs = require('fs'), zlib = require('zlib');
const [file, types, pat] = process.argv.slice(2), want = new Set(types.split(',')), re = new RegExp(pat, 'i');
const b = fs.readFileSync(file);
function edid(data) {
  for (let p = 0; p + 6 <= data.length;) {
    const t = data.toString('latin1', p, p + 4), n = data.readUInt16LE(p + 4);
    if (t === 'EDID') return data.toString('latin1', p + 6, p + 6 + n - 1);
    p += 6 + n;
  }
  return null;
}
function walk(start, end) {
  for (let p = start; p < end;) {
    const t = b.toString('latin1', p, p + 4), size = b.readUInt32LE(p + 4);
    if (t === 'GRUP') { walk(p + 24, p + size); p += size; continue; }
    if (want.has(t)) {
      const flags = b.readUInt32LE(p + 8), id = b.readUInt32LE(p + 12);
      let data = b.subarray(p + 24, p + 24 + size);
      if (flags & 0x40000) data = zlib.inflateSync(data.subarray(4));
      const e = edid(data);
      if (e && re.test(e)) console.log(`${t} ${id.toString(16).padStart(8, '0')} ${e}`);
    }
    p += 24 + size;
  }
}
walk(0, b.length);
