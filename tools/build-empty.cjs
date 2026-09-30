// Neutralises the retired base forms. Earlier test builds placed panel and light references that may still be in
// saves; the data-only plugin no longer manages them, so the panel model becomes an empty node and the light gets
// zero radius and fade. Output: dist/CausticLight/meshes/CausticLight/causticpanel.nif (empty).
'use strict';
const fs = require('fs'), path = require('path'), nif = require('./nif.cjs');
const OUT = process.argv[2] ?? 'dist/CausticLight/meshes/CausticLight/causticpanel.nif';
const t = nif.read(fs.readFileSync('reference/meshes/water/defaultwaterripples.nif'), { raw: true });
const blocks = [{ type: 'BSFadeNode', name: 0, extra: [], controller: -1, flags: 524302, translation: [0, 0, 0],
  rotation: [1, 0, 0, 0, 1, 0, 0, 0, 1], scale: 1, collision: -1, children: [], effects: [] }];
const buf = nif.write({ headerString: t.headerString, version: t.version, endian: t.endian, userVersion: t.userVersion,
  bsVersion: t.bsVersion, author: '', processScript: '', exportScript: '', strings: ['CausticPanelRetired'], groups: [], blocks, roots: [0] });
if (!nif.write(nif.read(buf)).equals(buf)) throw new Error('round-trip');
fs.mkdirSync(path.dirname(OUT), { recursive: true }); fs.writeFileSync(OUT, buf);
console.log(`${OUT}: ${buf.length} bytes, empty node`);
