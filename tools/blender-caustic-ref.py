# Physically based reference for the CausticLight draw pass: the same synthetic pond scene as
# plugin/tests/PassRender.cpp (scaled 0.01: 1 m = 100 Skyrim units), path traced by Cycles.
#
#   blender -b -P tools/blender-caustic-ref.py -- <out.exr> <water: glossy|black|refract> <amplitude m> [samples] [lamp radius m] [fill] [CPU|GPU] [lamp|sun|both] [sun deg] [sun x]
#   sun: a distant light toward (0.45, 0.4, 0.8) (normalised), 1 deg across, instead of the lamp (PassRender ref_mod_sun*.f32)
#   both: the lamp and the sun together (the sun's strength x [sun x]): two lights reflected at once (PassRender ref_mod_both_*.f32)
#
# Render the scene twice with the same ripples: water as a rippled mirror (glossy) and as a black
# absorber (black). glossy / black - 1 per pixel is the light the water reflects, as a fraction of
# the scene's other light: the quantity the mod adds (it multiplies the scene by 1 + boost).
#
# Scene (PassRender units / 100): pond (-2,-2)-(2,2) m at z 0 over a floor at -0.6; bridge x -2.3..2.3,
# y -0.4..0.4, underside 1.2, top 1.35; walls x = -2.3 (faces +x) and y = -2.3 (faces +y), up to 4.5 m.
# The north-east wall of PassRender is left out: it is never in view there and here it would shadow
# the pond from the lamp. Lamp at (6, 6, 4.5), radius 5 cm (a path tracer cannot sample a point light
# through a perfect mirror). A uniform grey world is the ambient fill (the Ragged Flagon walls are lit
# by many lights, not by the one lamp alone); its level is printed so results can be read against it.
# The world is hidden from glossy rays, so the water reflects only the lamp.
# Camera: eye (1.5, 1.2, 0.6) -> (-2.3, -2.0, 1.1), 70 deg vertical field of view, 640 x 400.
#
# Ripples: real displaced geometry, three sine waves of wavelength 1.68 m (the mod's pattern tile)
# at 0, 60 and 120 deg. Slope amplitude per wave = 2 pi A / lambda; the RMS slope is printed.
import bpy, bmesh, math, sys, mathutils

argv = sys.argv[sys.argv.index('--') + 1:]
out = argv[0]
water_mode = argv[1]
amp = float(argv[2])
samples = int(argv[3]) if len(argv) > 3 else 2048
lamp_radius = float(argv[4]) if len(argv) > 4 else 0.25
fill = float(argv[5]) if len(argv) > 5 else 0.25
use_sun = len(argv) > 7 and argv[7] in ('sun', 'both')
use_both = len(argv) > 7 and argv[7] == 'both'
sun_mult = float(argv[9]) if len(argv) > 9 else 1.0
# 'causticonly': the walls, floor and bridge hidden from glossy rays, so the water reflects the lights only (no mirror
# image of the lit walls on the underside): the caustics alone
caustic_only = len(argv) > 10 and argv[10] == 'causticonly'
sun_angle = float(argv[8]) if len(argv) > 8 else 1.0  # deg across; bigger converges faster (the mean is unchanged)
device = argv[6] if len(argv) > 6 else 'GPU'  # CPU for refraction: MNEE hit an illegal address on OptiX/CUDA in 5.2  # world strength (ambient); 0 = lamp only  # m; bigger converges faster, blurs the caustic by ~radius

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

def mat_diffuse(name, albedo):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = (albedo, albedo * 0.95, albedo * 0.9, 1)
    b.inputs['Roughness'].default_value = 1.0
    b.inputs['Specular IOR Level'].default_value = 0.0  # pure diffuse, like the game's rough stone
    return m

def quad(name, verts, mat):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], [(0, 1, 2, 3)])
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    ob.data.materials.append(mat)
    return ob

def box(name, x0, y0, z0, x1, y1, z1, mat):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x = x0 if v.co.x < 0 else x1
        v.co.y = y0 if v.co.y < 0 else y1
        v.co.z = z0 if v.co.z < 0 else z1
    bm.to_mesh(me)
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    ob.data.materials.append(mat)
    return ob

floor = mat_diffuse('floor', 0.35)
wall = mat_diffuse('wall', 0.6)
wood = mat_diffuse('wood', 0.5)
floor_ob = quad('floor', [(-2.3, -2.3, -0.6), (2.3, -2.3, -0.6), (2.3, 2.3, -0.6), (-2.3, 2.3, -0.6)], floor)
# walls as thin boxes so light cannot leak through their edges
box('wall_x', -2.4, -2.4, -0.6, -2.3, 2.3, 4.5, wall)
box('wall_y', -2.4, -2.4, -0.6, 2.3, -2.3, 4.5, wall)
box('bridge', -2.3, -0.4, 1.2, 2.3, 0.4, 1.35, wood)

# rippled water: 4 x 4 m, 1 cm grid
water = bpy.data.materials.new('water')
water.use_nodes = True
wb = water.node_tree.nodes['Principled BSDF']
wb.inputs['Base Color'].default_value = (0, 0, 0, 1)
wb.inputs['Roughness'].default_value = 0.02
wb.inputs['IOR'].default_value = 1.33
wb.inputs['Specular IOR Level'].default_value = 0.5 if water_mode in ('glossy', 'refract') else 0.0
if water_mode == 'refract':
    # clear water: the lamp's light passes the surface and is refracted onto the pool floor
    wb.inputs['Base Color'].default_value = (1, 1, 1, 1)
    wb.inputs['Transmission Weight'].default_value = 1.0
    wb.inputs['Roughness'].default_value = 0.0
wl = 1.68
dirs = [(math.cos(math.radians(a)), math.sin(math.radians(a))) for a in (0, 60, 120)]
phase = [0.0, 1.3, 2.1]
n = 400
me = bpy.data.meshes.new('water')
bm = bmesh.new()
bmesh.ops.create_grid(bm, x_segments=n, y_segments=n, size=2.0)
k = 2 * math.pi / wl
s2 = 0.0
for v in bm.verts:
    x, y = v.co.x, v.co.y
    v.co.z = sum(amp * math.sin(k * (d[0] * x + d[1] * y) + p) for d, p in zip(dirs, phase))
    gx = sum(amp * k * d[0] * math.cos(k * (d[0] * x + d[1] * y) + p) for d, p in zip(dirs, phase))
    gy = sum(amp * k * d[1] * math.cos(k * (d[0] * x + d[1] * y) + p) for d, p in zip(dirs, phase))
    s2 += gx * gx + gy * gy
for f in bm.faces:
    f.smooth = True
bm.to_mesh(me)
rms = math.degrees(math.atan(math.sqrt(s2 / len(bm.verts))))
wo = bpy.data.objects.new('water', me)
scene.collection.objects.link(wo)
wo.data.materials.append(water)
if water_mode == 'refract':
    wo.visible_camera = False               # the camera sees the floor as the mod does (no view refraction)
    wo.cycles.is_caustics_caster = True     # manifold next-event estimation: refracted light onto the floor
    floor_ob.cycles.is_caustics_receiver = True

# lamp
ld = bpy.data.lights.new('lamp', 'POINT')
ld.energy = 3000.0
ld.shadow_soft_size = lamp_radius
ld.use_soft_falloff = False  # physical inverse square
lo = bpy.data.objects.new('lamp', ld)
lo.location = (6.0, 6.0, 4.5)
scene.collection.objects.link(lo)
ld.cycles.is_caustics_light = True
if use_sun:
    if not use_both:
        scene.collection.objects.unlink(lo)
    ld = bpy.data.lights.new('sun', 'SUN')
    ld.energy = 3.0 * sun_mult  # W/m2 on a face square to it
    ld.angle = math.radians(sun_angle)
    lo = bpy.data.objects.new('sun', ld)
    to_sun = mathutils.Vector((0.45, 0.4, 0.8)).normalized()
    lo.rotation_euler = (-to_sun).to_track_quat('-Z', 'Y').to_euler()  # a sun light shines along its -Z
    scene.collection.objects.link(lo)
    ld.cycles.is_caustics_light = True

if caustic_only:
    for name in ('floor', 'wall_x', 'wall_y', 'bridge'):
        bpy.data.objects[name].visible_glossy = False

# ambient fill
world = bpy.data.worlds.new('fill')
world.use_nodes = True
world.node_tree.nodes['Background'].inputs['Color'].default_value = (1, 1, 1, 1)
world.node_tree.nodes['Background'].inputs['Strength'].default_value = fill
scene.world = world
# the water must reflect only the lamp (the mod reflects lights, not the ambient): hide the world from glossy rays
world.cycles_visibility.glossy = False

# camera
cd = bpy.data.cameras.new('cam')
cd.sensor_fit = 'VERTICAL'
cd.angle_y = math.radians(70)
cd.clip_start = 0.05
co = bpy.data.objects.new('cam', cd)
scene.collection.objects.link(co)
eye = (1.5, 1.2, 0.6)
tgt = (-2.3, -2.0, 1.1)
co.location = eye
import mathutils
direction = mathutils.Vector(tgt) - mathutils.Vector(eye)
co.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
scene.camera = co

# render
scene.render.engine = 'CYCLES'
prefs = bpy.context.preferences.addons['cycles'].preferences
prefs.compute_device_type = 'OPTIX'
prefs.get_devices()
for d in prefs.devices:
    d.use = d.type == 'OPTIX'
c = scene.cycles
c.device = device
c.samples = samples
c.use_adaptive_sampling = False
c.use_denoising = False
c.caustics_reflective = True
c.caustics_refractive = water_mode == 'refract'
c.blur_glossy = 0.0
c.sample_clamp_direct = 0.0
c.sample_clamp_indirect = 0.0
c.max_bounces = 6
c.seed = 7
scene.render.resolution_x = 640
scene.render.resolution_y = 400
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = 'Standard'
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.filepath = out
print(f'[ref] water {water_mode}, amplitude {amp:.4f} m, slope per wave {math.degrees(math.atan(k * amp)):.2f} deg, '
      f'RMS slope {rms:.2f} deg, lamp {ld.energy:.0f} W radius {lamp_radius} m, fill {fill}, samples {samples}')
bpy.ops.render.render(write_still=True)
print(f'[ref] wrote {out}')
