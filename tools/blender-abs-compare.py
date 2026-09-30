# The reflected light itself, not its share: Cycles glossy - black (the same fill and bounces cancel) against the
# mod's reflected term R = dump x (D + 1) (the dumps used ambient 1), converted to Cycles units by the light's own
# direct-light scale (blender-both-level.py). The strength that matches is fitted per surface group; the share
# compare (blender-caustic-compare.py) also carries the ambient, which it measures on the walls only.
#
#   blender -b -P tools/blender-abs-compare.py -- <ref dir> <PassRender dir> <dump name> <Cycles per mod unit>
import bpy, numpy as np, sys, os

argv = sys.argv[sys.argv.index('--') + 1:]
ref_dir, mod_dir, dump, scale = argv[0], argv[1], argv[2], float(argv[3])
W, H = 640, 400

def lum_exr(path):
    im = bpy.data.images.load(path)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(im.size[1], im.size[0], 4)[::-1, :, :3]
    bpy.data.images.remove(im)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

ids = np.fromfile(os.path.join(mod_dir, 'ref_ids.u32'), dtype=np.uint32).reshape(H, W)
interior = np.ones((H, W), bool)
for dy in (-1, 0, 1):
    for dx in (-1, 0, 1):
        interior &= np.roll(np.roll(ids, dy, 0), dx, 1) == ids
b = np.fromfile(os.path.join(mod_dir, f'ref_mod_{dump}.f32'), dtype=np.float32).reshape(H, W)
D = np.fromfile(os.path.join(mod_dir, f'ref_mod_{dump}_direct.f32'), dtype=np.float32).reshape(H, W)
Rm = b * (D + 1.0) * scale
Rp = lum_exr(os.path.join(ref_dir, 'calm_' + os.environ.get('CL_GLOSSY', 'glossy') + '.exr')) - lum_exr(os.path.join(ref_dir, 'calm_black.exr'))
out = []
for name, group in (('bridge underside', [2]), ('walls', [4, 5]), ('bridge sides', [7]), ('all', [2, 4, 5, 7])):
    sel = np.isin(ids, group) & interior
    p = np.clip(Rp[sel], None, np.percentile(Rp[sel], 99.5)).sum()
    m = Rm[sel].sum()
    out.append(f'{name} {p / m:.2f}' if m > 0 else f'{name} -')
print(f'[abs] {dump}: strength that matches the reflected light: ' + ', '.join(out))
