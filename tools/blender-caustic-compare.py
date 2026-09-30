# Compares the Cycles reference (tools/blender-caustic-ref.py) with the mod's own pass over the same
# scene (plugin/tests/PassRender.cpp dumps ref_*.f32 / ref_ids.u32).
#
#   blender -b -P tools/blender-caustic-compare.py -- <ref dir> <PassRender dir> <out.png>
#
# ref dir: {calm,rough}_glossy.exr, {calm,rough}_black.exr (fill on) and calm_black_nofill.exr (lamp only).
# Physical boost = glossy / black - 1 (luminance): the light the water reflects as a share of the light
# already on the surface. The mod computes boost = g x R / (D + A): R reflected, D the lamp's direct light
# (both from the dumps; R = dump x (D + 1) because the dump used A = 1), A the ambient in lamp units.
# A is measured from Cycles, so both see the same room: A/D = (black - black_nofill) / black_nofill on the
# faces the lamp lights directly, times the mod's D there. Then g (the strength) is fitted and reported.
import bpy, numpy as np, sys, os

argv = sys.argv[sys.argv.index('--') + 1:]
ref_dir, mod_dir, out_png = argv[0], argv[1], argv[2]
# optional 4th argument 'tides': compare the Surface Tides path (ref_mod_tides.f32, the same waves as the calm render)
# or 'sun': the sun renders (blender-caustic-ref.py ... sun) against ref_mod_sun.f32 / ref_mod_sun_direct.f32
mode = argv[3] if len(argv) > 3 else ''
# or 'mirror_vanilla' / 'mirror_isl': the C++ mirror's float dumps (pattern at its mean) with the lamp's vanilla or inverse-square falloff
CASES = [('calm', 'calm')] if mode == 'calm' else [('calm', 'tides')] if mode == 'tides' else [('calm', 'sun')] if mode == 'sun' else [('calm', mode)] if mode.startswith('mirror_') else [('calm', 'calm'), ('rough', 'rough')]
W, H = 640, 400
CAP = 2.8  # kMaxBoost (the walls; the floor keeps kMaxFloorBoost 1.5)

def load_exr(path):
    im = bpy.data.images.load(path)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(im.size[1], im.size[0], 4)[::-1, :, :3]  # row 0 = top
    bpy.data.images.remove(im)
    return a

def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

def tmean(x):
    # mean with the brightest 0.5 % clipped: path-traced caustics leave a few very bright specks
    return float(np.clip(x, None, np.percentile(x, 99.5)).mean()) if x.size else 0.0

ids = np.fromfile(os.path.join(mod_dir, 'ref_ids.u32'), dtype=np.uint32).reshape(H, W)
D = np.fromfile(os.path.join(mod_dir, 'ref_mod_sun_direct.f32' if mode == 'sun' else f'ref_mod_{mode}_direct.f32' if mode.startswith('mirror_') else 'ref_mod_direct.f32'), dtype=np.float32).reshape(H, W)
names = {2: 'bridge underside', 4: 'south-west wall (x)', 5: 'south-west wall (y)', 7: 'bridge sides'}
lit_ids = [2, 4, 5, 7]
interior = np.ones((H, W), bool)
for dy in (-1, 0, 1):
    for dx in (-1, 0, 1):
        interior &= np.roll(np.roll(ids, dy, 0), dx, 1) == ids

# the room's ambient in the mod's lamp units, from Cycles
B = lum(load_exr(os.path.join(ref_dir, 'calm_black.exr')))
B0 = lum(load_exr(os.path.join(ref_dir, 'calm_black_nofill.exr')))
direct_faces = np.isin(ids, lit_ids) & interior & (D > 0.1) & (B0 > 1e-3)
A_mod = float(np.median((B[direct_faces] - B0[direct_faces]) / B0[direct_faces] * D[direct_faces]))

def heat(b, vmax):
    t = np.clip(b / vmax, 0, 1)
    return np.stack([np.clip(1.5 * t, 0, 1), np.clip(1.5 * t - 0.5, 0, 1), np.clip(3 * t - 2, 0, 1) * 0.6 + 0.15 * (1 - t)], -1)

report = [f'ambient measured in Cycles: A = {A_mod:.3f} in lamp units (median over {int(direct_faces.sum())} directly lit pixels)']
rows = []
fits = []
for case, dump in CASES:
    A = lum(load_exr(os.path.join(ref_dir, f'{case}_' + os.environ.get('CL_GLOSSY', 'glossy') + '.exr')))
    Bc = lum(load_exr(os.path.join(ref_dir, f'{case}_black.exr')))
    ok = Bc > 1e-3
    phys = np.where(ok, A / np.maximum(Bc, 1e-6) - 1.0, 0.0)
    R = np.fromfile(os.path.join(mod_dir, f'ref_mod_{dump}.f32'), dtype=np.float32).reshape(H, W) * (D + 1.0)
    raw = R / (D + A_mod)  # the mod at strength 1, uncapped

    sel = np.isin(ids, lit_ids) & interior & ok
    g = tmean(phys[sel]) / max(float(raw[sel].mean()), 1e-9)
    fits.append(g)
    mod = np.minimum(raw * g, CAP)
    report.append(f'== {case} ({dump}): strength that matches the mean over walls + bridge = {g:.2f}  (mean* = brightest 0.5 % clipped)')
    report.append(f'   {"surface":22s} {"physical mean*/p95/lit>3%":>27s}   {"mod (fitted) mean/p95/lit>3%":>30s}   {"mod x1 mean":>11s}')
    for s in lit_ids:
        m = (ids == s) & interior & ok
        if m.sum() < 50:
            continue
        p, q = phys[m], mod[m]
        report.append(f'   {names[s]:22s} {100*tmean(p):7.2f}% {100*np.percentile(p,95):7.2f}% {100*(p>0.03).mean():6.1f}%   '
                      f'{100*q.mean():8.2f}% {100*np.percentile(q,95):8.2f}% {100*(q>0.03).mean():7.1f}%   {100*raw[m].mean():9.2f}%')
    tone = np.clip(np.stack([A] * 3, -1) / (1 + np.stack([A] * 3, -1)), 0, 1) ** (1 / 2.2)
    vmax = max(np.percentile(phys[sel], 99), 0.05)
    white = np.full((H, 6, 3), 1.0)
    rows.append(np.concatenate([tone, white, heat(phys, vmax), white, heat(mod, vmax)], 1))
    report.append(f'   heat maps scaled 0 .. {100*vmax:.0f} %')
report.append('strength fits: ' + ', '.join(f'{c} {g:.2f}' for (c, _), g in zip(CASES, fits)))

fig = rows[0] if len(rows) == 1 else np.concatenate([rows[0], np.full((6, rows[0].shape[1], 3), 1.0), rows[1]], 0)
h, w = fig.shape[:2]
img = bpy.data.images.new('fig', w, h, alpha=False)
img.pixels = np.concatenate([fig[::-1], np.ones((h, w, 1))], -1).astype(np.float32).ravel()
img.filepath_raw = out_png
img.file_format = 'PNG'
img.save()
print('\n'.join(report))
print(f'[compare] wrote {out_png}')
