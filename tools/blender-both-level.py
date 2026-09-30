# The sun's strength over the lamp's, in the mod's units, for the two-light compare (PassRender CL_BOTH_LEVEL):
# each single-light render's direct light (black water, no fill) over the mod's direct dump for that light, on
# the faces the light reaches directly; the ratio of the two scales is the sun's level in the lamp's units.
#
#   blender -b -P tools/blender-both-level.py -- <lamp ref dir> <sun ref dir> <PassRender dir> [sun x]
import bpy, numpy as np, sys, os

argv = sys.argv[sys.argv.index('--') + 1:]
lamp_dir, sun_dir, mod_dir = argv[0], argv[1], argv[2]
sun_mult = float(argv[3]) if len(argv) > 3 else 1.0  # the both render's sun strength over the sun render's
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
faces = np.isin(ids, [4, 5, 7]) & interior  # the walls and the bridge sides (not the underside: reflection only)

def scale(exr, dump):
    B0 = lum_exr(exr)
    D = np.fromfile(os.path.join(mod_dir, dump), dtype=np.float32).reshape(H, W)
    sel = faces & (D > 0.02 * D[faces].max()) & (B0 > 1e-4)
    r = B0[sel] / D[sel]
    print(f'[level] {dump}: {sel.sum()} px, Cycles / mod median {np.median(r):.5g} (p10 {np.percentile(r, 10):.5g}, p90 {np.percentile(r, 90):.5g})')
    return float(np.median(r))

c_lamp = scale(os.path.join(lamp_dir, 'calm_black_nofill.exr'), 'ref_mod_mirror_isl_direct.f32')
c_sun = scale(os.path.join(sun_dir, 'calm_black_nofill.exr'), 'ref_mod_sun_direct.f32')
print(f'[level] CL_BOTH_LEVEL={sun_mult * c_sun / c_lamp:.6g}')
