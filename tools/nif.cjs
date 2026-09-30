// Minimal reader/writer for Skyrim SE NIF files (20.2.0.7, user 12, BS 100): only the block types this mod uses.
// Every block type is decoded into fields and re-encoded; `node nif.cjs roundtrip <file>` proves the codec
// reproduces a file byte for byte before we trust it to write new ones.
'use strict';
const fs = require('fs');

class R {
  constructor(b) { this.b = b; this.p = 0; }
  u8() { return this.b.readUInt8(this.p++); }
  u16() { const v = this.b.readUInt16LE(this.p); this.p += 2; return v; }
  u32() { const v = this.b.readUInt32LE(this.p); this.p += 4; return v; }
  i32() { const v = this.b.readInt32LE(this.p); this.p += 4; return v; }
  f32() { const v = this.b.readFloatLE(this.p); this.p += 4; return v; }
  u64() { const v = this.b.readBigUInt64LE(this.p); this.p += 8; return v; }
  bytes(n) { const v = this.b.subarray(this.p, this.p + n); this.p += n; return Buffer.from(v); }
  fs(n) { return Array.from({ length: n }, () => this.f32()); }
  sized() { const n = this.u32(); return this.bytes(n).toString('latin1'); }
  short() { const n = this.u8(); return this.bytes(n).toString('latin1'); }
  refs() { const n = this.u32(); return Array.from({ length: n }, () => this.i32()); }
}
class W {
  constructor() { this.parts = []; }
  push(b) { this.parts.push(b); }
  u8(v) { const b = Buffer.alloc(1); b.writeUInt8(v); this.push(b); }
  u16(v) { const b = Buffer.alloc(2); b.writeUInt16LE(v); this.push(b); }
  u32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); this.push(b); }
  i32(v) { const b = Buffer.alloc(4); b.writeInt32LE(v); this.push(b); }
  f32(v) { const b = Buffer.alloc(4); b.writeFloatLE(v); this.push(b); }
  u64(v) { const b = Buffer.alloc(8); b.writeBigUInt64LE(v); this.push(b); }
  bytes(b) { this.push(Buffer.from(b)); }
  fs(a) { a.forEach(v => this.f32(v)); }
  sized(s) { const b = Buffer.from(s, 'latin1'); this.u32(b.length); this.push(b); }
  short(s) { const b = Buffer.from(s, 'latin1'); this.u8(b.length); this.push(b); }
  refs(a) { this.u32(a.length); a.forEach(v => this.i32(v)); }
  buf() { return Buffer.concat(this.parts); }
}

// ---------------------------------------------------------------- block codecs
// NiObjectNET: name (string index), extra data refs, controller ref
const rNET = (r, o) => { o.name = r.i32(); o.extra = r.refs(); o.controller = r.i32(); };
const wNET = (w, o) => { w.i32(o.name); w.refs(o.extra); w.i32(o.controller); };
// NiAVObject (BS 100): flags u32, translation, rotation 3x3, scale, collision ref
const rAV = (r, o) => { rNET(r, o); o.flags = r.u32(); o.translation = r.fs(3); o.rotation = r.fs(9); o.scale = r.f32(); o.collision = r.i32(); };
const wAV = (w, o) => { wNET(w, o); w.u32(o.flags); w.fs(o.translation); w.fs(o.rotation); w.f32(o.scale); w.i32(o.collision); };
// NiTimeController
const rTC = (r, o) => { o.next = r.i32(); o.flags = r.u16(); o.frequency = r.f32(); o.phase = r.f32(); o.start = r.f32(); o.stop = r.f32(); o.target = r.i32(); };
const wTC = (w, o) => { w.i32(o.next); w.u16(o.flags); w.f32(o.frequency); w.f32(o.phase); w.f32(o.start); w.f32(o.stop); w.i32(o.target); };

// vertex format: attribute flags live in bits 44..55 of the vertex descriptor
const VA = { vertex: 0x1, uv: 0x2, uv2: 0x4, normal: 0x8, tangent: 0x10, colors: 0x20, skinned: 0x40, land: 0x80, eye: 0x100, full: 0x400 };
function vertexLayout(desc) {
  const a = Number((desc >> 44n) & 0xfffn), size = Number(desc & 0xfn) * 4;
  return { a, size, has: k => (a & VA[k]) !== 0 };
}

const CODECS = {
  NiNode: {
    read(r, o) { rAV(r, o); o.children = r.refs(); o.effects = r.refs(); },
    write(w, o) { wAV(w, o); w.refs(o.children); w.refs(o.effects); },
  },
  BSXFlags: {
    read(r, o) { o.name = r.i32(); o.value = r.u32(); },
    write(w, o) { w.i32(o.name); w.u32(o.value); },
  },
  NiStringExtraData: {
    read(r, o) { o.name = r.i32(); o.value = r.i32(); },
    write(w, o) { w.i32(o.name); w.i32(o.value); },
  },
  BSTriShape: {
    read(r, o) {
      rAV(r, o); o.center = r.fs(3); o.radius = r.f32(); o.skin = r.i32(); o.shader = r.i32(); o.alpha = r.i32();
      o.vertexDesc = r.u64(); o.numTris = r.u16(); o.numVerts = r.u16(); o.dataSize = r.u32();
      const L = vertexLayout(o.vertexDesc);
      if (o.dataSize > 0) {
        if (L.size * o.numVerts + 6 * o.numTris !== o.dataSize) throw new Error('BSTriShape: data size mismatch');
        o.vertices = Array.from({ length: o.numVerts }, () => r.bytes(L.size));
        o.tris = Array.from({ length: o.numTris }, () => [r.u16(), r.u16(), r.u16()]);
      } else { o.vertices = []; o.tris = []; }
      o.particleDataSize = r.u32();
      if (o.particleDataSize > 0) o.particleData = r.bytes(o.particleDataSize * 0);   // not used by these meshes
    },
    write(w, o) {
      wAV(w, o); w.fs(o.center); w.f32(o.radius); w.i32(o.skin); w.i32(o.shader); w.i32(o.alpha);
      const L = vertexLayout(o.vertexDesc);
      w.u64(o.vertexDesc); w.u16(o.tris.length); w.u16(o.vertices.length);
      w.u32(o.vertices.length ? L.size * o.vertices.length + 6 * o.tris.length : 0);
      o.vertices.forEach(v => { if (v.length !== L.size) throw new Error('vertex size'); w.bytes(v); });
      o.tris.forEach(t => t.forEach(i => w.u16(i)));
      if (o.particleDataSize) throw new Error('particle data not supported');
      w.u32(0);
    },
  },
  BSEffectShaderProperty: {
    read(r, o) {
      rNET(r, o); o.flags1 = r.u32(); o.flags2 = r.u32(); o.uvOffset = r.fs(2); o.uvScale = r.fs(2);
      o.texture = r.sized(); o.clamp = r.u8(); o.lightingInfluence = r.u8(); o.envMapMinLod = r.u8(); o.unused = r.u8();
      o.falloff = r.fs(4); o.baseColor = r.fs(4); o.baseColorScale = r.f32(); o.softFalloffDepth = r.f32(); o.greyscale = r.sized();
    },
    write(w, o) {
      wNET(w, o); w.u32(o.flags1); w.u32(o.flags2); w.fs(o.uvOffset); w.fs(o.uvScale);
      w.sized(o.texture); w.u8(o.clamp); w.u8(o.lightingInfluence); w.u8(o.envMapMinLod); w.u8(o.unused);
      w.fs(o.falloff); w.fs(o.baseColor); w.f32(o.baseColorScale); w.f32(o.softFalloffDepth); w.sized(o.greyscale);
    },
  },
  NiAlphaProperty: {
    read(r, o) { rNET(r, o); o.flags = r.u16(); o.threshold = r.u8(); },
    write(w, o) { wNET(w, o); w.u16(o.flags); w.u8(o.threshold); },
  },
  BSEffectShaderPropertyFloatController: {
    read(r, o) { rTC(r, o); o.interpolator = r.i32(); o.variable = r.u32(); },
    write(w, o) { wTC(w, o); w.i32(o.interpolator); w.u32(o.variable); },
  },
  BSEffectShaderPropertyColorController: {
    read(r, o) { rTC(r, o); o.interpolator = r.i32(); o.variable = r.u32(); },
    write(w, o) { wTC(w, o); w.i32(o.interpolator); w.u32(o.variable); },
  },
  NiFloatInterpolator: {
    read(r, o) { o.value = r.f32(); o.data = r.i32(); },
    write(w, o) { w.f32(o.value); w.i32(o.data); },
  },
  NiPoint3Interpolator: {
    read(r, o) { o.value = r.fs(3); o.data = r.i32(); },
    write(w, o) { w.fs(o.value); w.i32(o.data); },
  },
  // key groups: count, then (if any) interpolation type and keys; quadratic keys carry forward/backward tangents
  NiFloatData: {
    read(r, o) { readKeys(r, o, 1); },
    write(w, o) { writeKeys(w, o, 1); },
  },
  NiPosData: {
    read(r, o) { readKeys(r, o, 3); },
    write(w, o) { writeKeys(w, o, 3); },
  },
};
function readKeys(r, o, dim) {
  const n = r.u32(); o.keys = [];
  if (n === 0) { o.interp = 0; return; }
  o.interp = r.u32();
  for (let i = 0; i < n; i++) {
    const k = { t: r.f32(), v: r.fs(dim) };
    if (o.interp === 2) { k.fwd = r.fs(dim); k.bwd = r.fs(dim); }
    else if (o.interp === 3) k.tbc = r.fs(3);
    else if (o.interp !== 1 && o.interp !== 5) throw new Error('key type ' + o.interp);
    o.keys.push(k);
  }
}
function writeKeys(w, o, dim) {
  w.u32(o.keys.length);
  if (!o.keys.length) return;
  w.u32(o.interp);
  for (const k of o.keys) { w.f32(k.t); w.fs(k.v); if (o.interp === 2) { w.fs(k.fwd); w.fs(k.bwd); } else if (o.interp === 3) w.fs(k.tbc); }
}

// ---------------------------------------------------------------- file
function read(buf, { raw = false } = {}) {
  const r = new R(buf);
  let nl = buf.indexOf(10); const headerString = buf.subarray(0, nl).toString('latin1'); r.p = nl + 1;
  const h = { headerString, version: r.u32(), endian: r.u8(), userVersion: r.u32() };
  const nb = r.u32(); h.bsVersion = r.u32();
  if (h.version !== 0x14020007 || h.bsVersion !== 100) throw new Error(`not an SSE NIF (bs ${h.bsVersion})`);
  h.author = r.short(); h.processScript = r.short(); h.exportScript = r.short();
  const types = Array.from({ length: r.u16() }, () => r.sized());
  const tidx = Array.from({ length: nb }, () => r.u16());
  const sizes = Array.from({ length: nb }, () => r.u32());
  const nstr = r.u32(); r.u32();
  h.strings = Array.from({ length: nstr }, () => r.sized());
  h.groups = r.refs();
  h.blocks = [];
  for (let i = 0; i < nb; i++) {
    const type = types[tidx[i] & 0x7fff], start = r.p, end = start + sizes[i], raw0 = buf.subarray(start, end);
    const o = { type };
    if (!raw && CODECS[type]) {
      const br = new R(raw0); CODECS[type].read(br, o);
      if (br.p !== raw0.length) throw new Error(`[${i}] ${type}: read ${br.p} of ${raw0.length} bytes`);
    } else o.raw = Buffer.from(raw0);
    h.blocks.push(o); r.p = end;
  }
  h.roots = r.refs();
  if (r.p !== buf.length) throw new Error('trailing bytes');
  return h;
}
function write(h) {
  const w = new W(), types = [];
  const blocks = h.blocks.map(o => {
    if (!types.includes(o.type)) types.push(o.type);
    if (o.raw) return o.raw;
    if (!CODECS[o.type]) throw new Error('no codec for ' + o.type);
    const bw = new W(); CODECS[o.type].write(bw, o); return bw.buf();
  });
  w.bytes(Buffer.from(h.headerString + '\n', 'latin1'));
  w.u32(h.version); w.u8(h.endian); w.u32(h.userVersion); w.u32(blocks.length); w.u32(h.bsVersion);
  w.short(h.author); w.short(h.processScript); w.short(h.exportScript);
  w.u16(types.length); types.forEach(t => w.sized(t));
  h.blocks.forEach(o => w.u16(types.indexOf(o.type)));
  blocks.forEach(b => w.u32(b.length));
  w.u32(h.strings.length); w.u32(Math.max(0, ...h.strings.map(s => s.length)));
  h.strings.forEach(s => w.sized(s));
  w.refs(h.groups);
  blocks.forEach(b => w.bytes(b));
  w.refs(h.roots);
  return w.buf();
}

CODECS.BSFadeNode = CODECS.NiNode;
CODECS.NiBooleanExtraData = { read(r, o) { o.name = r.i32(); o.value = r.u8(); }, write(w, o) { w.i32(o.name); w.u8(o.value); } };

module.exports = { read, write, vertexLayout, VA };

if (require.main === module) {
  const [cmd, ...files] = process.argv.slice(2);
  for (const f of files) {
    const buf = fs.readFileSync(f), h = read(buf);
    if (cmd === 'roundtrip') {
      const out = write(h), same = out.equals(buf);
      const unknown = [...new Set(h.blocks.filter(b => b.raw).map(b => b.type))];
      console.log(`${same ? 'SAME ' : 'DIFF '} ${f}${unknown.length ? '  (kept raw: ' + unknown.join(', ') + ')' : ''}`);
      if (!same) process.exitCode = 1;
    } else if (cmd === 'dump') {
      console.log(`${f}  strings=${JSON.stringify(h.strings)}`);
      h.blocks.forEach((b, i) => {
        const o = { ...b }; delete o.type;
        if (o.vertices) { const L = vertexLayout(o.vertexDesc); o.vertices = `${o.vertices.length} x ${L.size} B (attrs 0x${L.a.toString(16)})`; o.vertexDesc = '0x' + o.vertexDesc.toString(16); o.tris = o.tris.length; }
        if (o.raw) o.raw = `${o.raw.length} B raw`;
        for (const k of ['flags1', 'flags2']) if (k in o) o[k] = '0x' + o[k].toString(16);
        if (b.type === 'NiAlphaProperty' || b.type.endsWith('Controller')) o.flags = '0x' + o.flags.toString(16);
        if (o.keys) o.keys = o.keys.slice(0, 6).map(k => `${+k.t.toFixed(4)}:${k.v.map(v => +v.toFixed(4))}`).join(' ') + (b.keys.length > 6 ? ` ...(${b.keys.length})` : '');
        console.log(`  [${i}] ${b.type} ${JSON.stringify(o)}`);
      });
    }
  }
}
