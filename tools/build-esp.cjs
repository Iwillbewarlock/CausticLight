// Writes CausticLight.esp (ESL-flagged, no masters). Since the data-only plugin (lighting-shader caustics) the two
// base forms are retired: the panel model is an empty node and the light is dark, so references that earlier test
// builds left in saves show nothing. Kept so those saves load without missing-form warnings.
// Former roles:
//   0x800 CL_CausticPanel  MSTT  meshes\CausticLight\causticpanel.nif
//   0x801 CL_CausticLight  LIGH  omni, no shadow, no model (the plugin sets colour, radius and fade at run time)
// Placement lives in the plugin's zones.json, so the file never edits a cell.
'use strict';
const fs = require('fs'), path = require('path');
const OUT = process.argv[2] ?? 'dist/CausticLight/CausticLight.esp';
const FORM_VERSION = 44;   // Skyrim SE

const sub = (type, data) => { const h = Buffer.alloc(6); h.write(type, 0, 'latin1'); h.writeUInt16LE(data.length, 4); return Buffer.concat([h, data]); };
const zstr = s => Buffer.from(s + '\0', 'latin1');
const u8 = v => Buffer.from([v]);
const i16s = a => { const b = Buffer.alloc(2 * a.length); a.forEach((v, i) => b.writeInt16LE(v, 2 * i)); return b; };
function record(type, flags, formId, subs) {
  const data = Buffer.concat(subs), h = Buffer.alloc(24);
  h.write(type, 0, 'latin1'); h.writeUInt32LE(data.length, 4); h.writeUInt32LE(flags, 8); h.writeUInt32LE(formId, 12);
  h.writeUInt32LE(0, 16); h.writeUInt16LE(FORM_VERSION, 20); h.writeUInt16LE(0, 22);
  return Buffer.concat([h, data]);
}
function group(label, records) {
  const body = Buffer.concat(records), h = Buffer.alloc(24);
  h.write('GRUP', 0, 'latin1'); h.writeUInt32LE(body.length + 24, 4); h.write(label, 8, 'latin1'); h.writeInt32LE(0, 12);
  return Buffer.concat([h, body]);
}

// LIGH DATA (48 bytes): time, radius, colour RGBA, flags, falloff exponent, FOV, near clip,
// flicker period, flicker intensity amplitude, flicker movement amplitude, value, weight
function lightData({ radius, color, flags }) {
  const b = Buffer.alloc(48);
  b.writeInt32LE(-1, 0); b.writeUInt32LE(radius, 4);
  b[8] = color[0]; b[9] = color[1]; b[10] = color[2]; b[11] = 0;
  b.writeUInt32LE(flags, 12); b.writeFloatLE(1, 16); b.writeFloatLE(90, 20); b.writeFloatLE(0, 24);
  b.writeFloatLE(0, 28); b.writeFloatLE(0, 32); b.writeFloatLE(0, 36); b.writeUInt32LE(0, 40); b.writeFloatLE(0, 44);
  return b;
}

const light = record('LIGH', 0, 0x801, [
  sub('EDID', zstr('CL_CausticLight')),
  sub('OBND', i16s([0, 0, 0, 0, 0, 0])),
  sub('DATA', lightData({ radius: 0, color: [0, 0, 0], flags: 0 })),   // retired: references left in old saves stay dark
  sub('FNAM', (() => { const b = Buffer.alloc(4); b.writeFloatLE(0, 0); return b; })()),
]);
const panel = record('MSTT', 0, 0x800, [
  sub('EDID', zstr('CL_CausticPanel')),
  sub('OBND', i16s([-168, -2, 0, 168, 2, 168])),
  sub('MODL', zstr('CausticLight\\causticpanel.nif')),
  sub('DATA', u8(0)),
]);
// console controls, read by the plugin a few times a second (no hotkeys: they clash with other mods)
function glob(id, edid, value) {
  const v = Buffer.alloc(4); v.writeFloatLE(value, 0);
  return record('GLOB', 0, id, [sub('EDID', zstr(edid)), sub('FNAM', Buffer.from('f', 'latin1')), sub('FLTV', v)]);
}
const globals = [
  glob(0x802, 'CL_Enabled', 1),    // 0 = do not draw
  glob(0x803, 'CL_DebugTint', 0),  // 1 = flat tint over each zone instead of caustics, 2 = where the reflected light lands, 3 = direct light from the lamps
  glob(0x804, 'CL_Corner', 0),     // set to 1: water-rect corner at the crosshair (twice = a zone); the plugin resets it to 0
  glob(0x805, 'CL_Remove', 0),     // set to 1: remove the zone at the crosshair; reset to 0
  glob(0x806, 'CL_Strength', 6.4),  // caustic strength (0..50); 6.4 = chosen to show, 3 x the physical match to Cycles (2.14)
];
const groups = [group('GLOB', globals), group('LIGH', [light]), group('MSTT', [panel])];

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(1.7, 0); hedr.writeInt32LE(10, 4); hedr.writeUInt32LE(0x807, 8);   // version, records + groups, next object id
const tes4 = record('TES4', 0x200, 0, [sub('HEDR', hedr), sub('CNAM', zstr('qtuna'))]);   // 0x200: light (ESL) flag

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, Buffer.concat([tes4, ...groups]));
console.log(`${OUT}: ${fs.statSync(OUT).size} bytes`);
