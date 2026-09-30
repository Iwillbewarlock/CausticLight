# Distribution check of the mod against the Cycles reference: not only the mean (blender-caustic-compare.py fits
# the strength to it) but how much the light varies around it (the pattern's contrast) and how bright the
# brightest streaks get (the cap, kMaxBoost).
#
#   blender -b -P tools/blender-caustic-stats.py -- <ref dir> <PassRender dir> <case>
#
# case: calm | rough | tides | sun   (the same renders / dumps as blender-caustic-compare.py; sun uses the sun dumps)
# Both images are box-filtered 3 x 3 first: Cycles leaves single-pixel speckles that are sampling noise, not
# caustics; the same filter on the mod keeps the comparison fair.
# Printed per surface, over the pixels where either is lit (> 1 %):
#   mean, cv = std / mean (the variation around the mean), p95 / p99 / p99.9
# and the suggestions:
#   contrast = kPatternContrast x cv_physical / cv_mod   (the mod's variation scales with its pattern contrast)
#   cap      = the physical p99.9 of the brightest surface (kMaxBoost clips anything above it)
import bpy, numpy as np, sys, os

argv = sys.argv[sys.argv.index('--') + 1:]
ref_dir, mod_dir, case = argv[0], argv[1], argv[2]
W, H = 640, 400
CONTRAST = 1.0  # kPatternContrast at the time of the dumps (the walls' pattern as it is since the reflection atlases)

def load_exr(path):
    im = bpy.data.images.load(path)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(im.size[1], im.size[0], 4)[::-1, :, :3]
    bpy.data.images.remove(im)
    return a

def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

def box3(x):
    p = np.pad(x, 1, mode='edge')
    return sum(p[1 + dy:1 + dy + H, 1 + dx:1 + dx + W] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0

ids = np.fromfile(os.path.join(mod_dir, 'ref_ids.u32'), dtype=np.uint32).reshape(H, W)
sun = case == 'sun'
D = np.fromfile(os.path.join(mod_dir, 'ref_mod_sun_direct.f32' if sun else 'ref_mod_direct.f32'), dtype=np.float32).reshape(H, W)
dump = {'calm': 'calm', 'rough': 'rough', 'tides': 'tides', 'sun': 'sun'}[case]
render = 'rough' if case == 'rough' else 'calm'
names = {2: 'bridge underside', 4: 'south-west wall (x)', 5: 'south-west wall (y)', 7: 'bridge sides'}
lit_ids = [2, 4, 5, 7]
interior = np.ones((H, W), bool)
for dy in (-2, -1, 0, 1, 2):
    for dx in (-2, -1, 0, 1, 2):
        interior &= np.roll(np.roll(ids, dy, 0), dx, 1) == ids  # 2 px from any edge: the 3 x 3 filter stays on one face

B = lum(load_exr(os.path.join(ref_dir, 'calm_black.exr')))
B0 = lum(load_exr(os.path.join(ref_dir, 'calm_black_nofill.exr')))
direct_faces = np.isin(ids, lit_ids) & interior & (D > 0.1) & (B0 > 1e-3)
A_mod = float(np.median((B[direct_faces] - B0[direct_faces]) / B0[direct_faces] * D[direct_faces]))

G = lum(load_exr(os.path.join(ref_dir, f'{render}_' + os.environ.get('CL_GLOSSY', 'glossy') + '.exr')))
Bc = lum(load_exr(os.path.join(ref_dir, f'{render}_black.exr')))
ok = Bc > 1e-3
phys = box3(np.where(ok, G / np.maximum(Bc, 1e-6) - 1.0, 0.0))
R = np.fromfile(os.path.join(mod_dir, f'ref_mod_{dump}.f32'), dtype=np.float32).reshape(H, W) * (D + 1.0)
raw = box3(R / (D + A_mod))
sel = np.isin(ids, lit_ids) & interior & ok
g = float(phys[sel].mean()) / max(float(raw[sel].mean()), 1e-9)
mod = raw * g  # fitted strength, no cap

print(f'== {case}: ambient A = {A_mod:.3f} lamp units, fitted strength {g:.2f} (the mod uncapped below)')
print(f'   {"surface":20s} {"":8s} {"mean":>7s} {"cv":>6s} {"p95":>7s} {"p99":>7s} {"p99.9":>7s}')
cvs = []
top = 0.0
for s in lit_ids:
    m = (ids == s) & interior & ok & ((phys > 0.01) | (mod > 0.01))
    if m.sum() < 100:
        continue
    for label, x in (('physical', phys[m]), ('mod', mod[m])):
        mean = float(x.mean())
        cv = float(x.std() / max(mean, 1e-9))
        p95, p99, p999 = (float(np.percentile(x, q)) for q in (95, 99, 99.9))
        print(f'   {names[s]:20s} {label:8s} {100*mean:6.2f}% {cv:6.2f} {100*p95:6.1f}% {100*p99:6.1f}% {100*p999:6.1f}%')
        if label == 'physical':
            cvp = cv
            top = max(top, p999)
        else:
            cvs.append((names[s], cvp, cv, int(m.sum())))
w = sum(n for _, _, _, n in cvs)
cv_phys = sum(a * n for _, a, _, n in cvs) / max(w, 1)
cv_mod = sum(b * n for _, _, b, n in cvs) / max(w, 1)
print(f'   weighted cv: physical {cv_phys:.2f}, mod {cv_mod:.2f} -> contrast that matches: {CONTRAST * cv_phys / max(cv_mod, 1e-9):.2f} (now {CONTRAST})')
print(f'   brightest physical p99.9: +{100*top:.0f} % -> a cap below this clips real streaks (kMaxBoost now +280 %)')
