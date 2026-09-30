# Pool-floor caustics: Cycles reference vs the mod.
#
#   blender -b -P tools/blender-floor-compare.py -- <ref dir> <PassRender dir> <out.png>
#
# ref dir: floor_rip.exr and floor_flat.exr (tools/blender-caustic-ref.py ... refract <amp> ... CPU):
# clear water that refracts the lamp's light onto the floor, rippled vs flat. rip / flat - 1 on the floor
# is how the ripples bunch the same light into streaks (the mean stays ~0).
# The mod: ref_mod_calm.f32 on the floor pixels, dumped at ambient 1 and strength 1; re-applied with the
# Cycles ambient (A, as in blender-caustic-compare.py) and at the default strength.
# Reports mean / spread (standard deviation) / 5th and 95th percentiles for both, and the floor contrast
# that makes the mod's spread match (the mod's depth absorption taken out: Cycles water is clear).
import bpy, numpy as np, sys, os, math

argv = sys.argv[sys.argv.index('--') + 1:]
ref_dir, mod_dir, out_png = argv[0], argv[1], argv[2]
DUMP = argv[3] if len(argv) > 3 else 'calm'  # 'tides': the Surface Tides path
W, H = 640, 400
A_MOD = 0.085              # ambient in lamp units, measured by blender-caustic-compare.py
STRENGTH = 2.14            # kDefaultStrength (the floor term is strength / kDefaultStrength: 1 at the default)
CONTRAST = 3.2             # kBottomContrast (x depth / focal length) used for the dump
ABSORB = math.exp(-60.0 / 300.0)  # the mod's absorption at this floor's depth (60 units, e-fold 300)

def load_exr(path):
    im = bpy.data.images.load(path)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(im.size[1], im.size[0], 4)[::-1, :, :3]
    bpy.data.images.remove(im)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

ids = np.fromfile(os.path.join(mod_dir, 'ref_ids.u32'), dtype=np.uint32).reshape(H, W)
D = np.fromfile(os.path.join(mod_dir, 'ref_mod_direct.f32'), dtype=np.float32).reshape(H, W)
interior = np.ones((H, W), bool)
for dy in (-2, 0, 2):
    for dx in (-2, 0, 2):
        interior &= np.roll(np.roll(ids, dy, 0), dx, 1) == ids
rip, flat = load_exr(os.path.join(ref_dir, 'floor_rip.exr')), load_exr(os.path.join(ref_dir, 'floor_flat.exr'))
# the pool floor inside the water rect (the floor also runs under the dry border of the room)
floor = (ids == 1) & interior & (flat > 1e-3)
phys = np.where(floor, rip / np.maximum(flat, 1e-6) - 1.0, 0.0)
F = np.fromfile(os.path.join(mod_dir, f'ref_mod_{DUMP}.f32'), dtype=np.float32).reshape(H, W) * (D + 1.0)
mod = np.clip(F / (D + A_MOD) * STRENGTH, -0.3, 0.6)  # the dump used strength 1: the floor term is strength / default
# only where the mod draws a floor (water above it)
sel = floor & (np.abs(F) > 1e-6)

def stats(x):
    return x.mean(), x.std(), np.percentile(x, 5), np.percentile(x, 95)

pm, ps, p5, p95 = stats(phys[sel])
mm, ms, m5, m95 = stats(mod[sel])
fit = CONTRAST * ps / max(ms / ABSORB, 1e-9)
print(f'floor pixels compared: {int(sel.sum())}')
print(f'physical (clear water): mean {100*pm:+.2f} %  spread {100*ps:.2f} %  p5 {100*p5:+.2f} %  p95 {100*p95:+.2f} %')
print(f'mod (contrast {CONTRAST}, absorb {ABSORB:.2f}): mean {100*mm:+.2f} %  spread {100*ms:.2f} %  p5 {100*m5:+.2f} %  p95 {100*m95:+.2f} %')
print(f'kBottomContrast that matches the physical spread (clear water): {fit:.3f} (x depth / focal length)')

def div(b, v):
    t = np.clip(b / v, -1, 1)
    return np.stack([np.clip(t, 0, 1), np.full_like(t, 0.0) + 0.15 * (1 - np.abs(t)), np.clip(-t, 0, 1)], -1)

v = max(np.percentile(np.abs(phys[sel]), 98), 0.02)
white = np.full((H, 6, 3), 1.0)
tone = np.clip(np.stack([rip] * 3, -1) / (1 + np.stack([rip] * 3, -1)), 0, 1) ** (1 / 2.2)
fig = np.concatenate([tone, white, div(np.where(sel, phys, 0), v), white, div(np.where(sel, mod * (fit / CONTRAST), 0), v)], 1)
h, w = fig.shape[:2]
img = bpy.data.images.new('fig', w, h, alpha=False)
img.pixels = np.concatenate([fig[::-1], np.ones((h, w, 1))], -1).astype(np.float32).ravel()
img.filepath_raw = out_png
img.file_format = 'PNG'
img.save()
print(f'maps: red = brighter, blue = darker, scaled +-{100*v:.0f} % | wrote {out_png}')
