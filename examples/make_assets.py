"""Procedurally build Patina's test assets and export them as .glb.

Run (from the repo root):
    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        --python examples/make_assets.py -- --out examples/assets

Options (after `--`):
    --out DIR                 output directory (default: examples/assets next to this script)
    --only crate,barrel,...   build a subset
    --uv-mode per-material    (default) every material / texture set gets its own full 0-1 UV
                              space (Substance-style texture sets; islands of *different*
                              materials may overlap each other, never within one material)
    --uv-mode shared          one packed 0-1 layout for the whole object, no overlaps at all
    --no-check                skip the UV rasterisation overlap check (and the angle fallback)

Assets (all +Y up in glTF, modifiers applied, split/custom normals, TEXCOORD_0, materials):
    crate.glb    beveled paneled box, convex + concave edges       material: Crate
    barrel.glb   stylized coopered barrel: 14 staves, board heads, riveted hoops  materials: Staves, Heads, Hoops, Rivets
    beer_barrel.glb  the barrel on its side in a wooden cradle with a brass tap   + materials: Tap, Stand
    hammer.glb   two objects: Handle + Head                         materials: Wood, Steel
    suzanne.glb  Suzanne, subdivision level 2 applied, smooth       material: Skin
    panel.glb    sci-fi panel with recessed grooves + bolts         material: Panel
    boltplate_low.glb / boltplate_high.glb   normal-bake test pair: hard-edged Plate_low + Bar_low (touching),
                 beveled Plate_high (raised panel, hex bolts) + Bar_high (bolts); materials: Plate, Bar

UVs:
  * hard-surface / organic meshes: Smart UV Project (angle 66, island margin 0.02) is used to
    segment islands, the island borders become seams, islands are relaxed with the
    Minimum-Stretch unwrapper (removes the projection stretch of faces seen at an angle) and
    then packed (rotation on, margin 0.006 of UV space). The result is rasterised; if any
    texel is covered twice inside one material the whole thing is redone with a lower angle
    limit (66 -> 55 -> 45 -> 35).
  * lathe meshes (barrel hoops, hammer handle) get analytic cylindrical UVs (arc length x profile
    length, planar lids), which pack far better than Smart UV's thin annuli, then are packed the same way.
  * wood boards (barrel staves and heads) are unfolded lengthwise along V and packed without
    rotation, so UV-space grain ("space": "uv" noise stretched "up") follows every board.

The script exits with a nonzero status if anything fails (Blender itself would exit 0 on an
uncaught Python exception).
"""

import argparse
import json
import math
import os
import struct
import sys
import traceback

import bpy
import bmesh
from mathutils import Matrix, Vector

ASSET_NAMES = ("crate", "barrel", "beer_barrel", "hammer", "suzanne", "panel", "boltplate_low", "boltplate_high", "boltplate_cage")

UV_ANGLES = (66.0, 55.0, 45.0, 35.0)   # smart-project angle limits tried in order
UV_ISLAND_MARGIN = 0.02                # smart project margin
UV_PACK_MARGIN = 0.006                 # pack margin, fraction of UV space (~6px @1k, 12px @2k)

OPTS = {"uv_mode": "per-material", "check": True}   # set from the command line in main()


# --------------------------------------------------------------------------------------
# generic helpers
# --------------------------------------------------------------------------------------

def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(prog="make_assets.py",
                                description="Build Patina test assets (.glb) procedurally.")
    p.add_argument("--out", default=os.path.join(here, "assets"))
    p.add_argument("--only", default="", help="comma separated subset of: " + ",".join(ASSET_NAMES))
    p.add_argument("--uv-mode", choices=("per-material", "shared"), default="per-material")
    p.add_argument("--no-check", action="store_true", help="skip UV overlap rasterisation check")
    return p.parse_args(argv)


def reset_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.cameras, bpy.data.lights,
                 bpy.data.images, bpy.data.curves):
        for d in list(coll):
            coll.remove(d)
    bpy.context.view_layer.update()


def make_material(name, color, metallic=0.0, roughness=0.5):
    m = bpy.data.materials.new(name)
    if m.node_tree is None:          # pre-5.0 behaviour
        m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    m.diffuse_color = (*color, 1.0)
    m.metallic = metallic
    m.roughness = roughness
    return m


def object_from_bmesh(name, bm, materials):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for m in materials:
        me.materials.append(m)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def add_bevel(obj, width, segments, angle_deg=30.0):
    obj.data.shade_smooth()          # harden_normals needs smooth faces
    mod = obj.modifiers.new("Bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    mod.angle_limit = math.radians(angle_deg)
    mod.use_clamp_overlap = True
    mod.harden_normals = True        # flat faces stay flat, bevels get smooth custom normals
    return mod


def apply_modifiers(obj):
    """Bake the modifier stack into the mesh (context free, keeps custom normals + materials)."""
    if not obj.modifiers:
        return
    dg = bpy.context.evaluated_depsgraph_get()
    new_me = bpy.data.meshes.new_from_object(obj.evaluated_get(dg), preserve_all_data_layers=True,
                                             depsgraph=dg)
    old = obj.data
    obj.modifiers.clear()
    obj.data = new_me
    bpy.data.meshes.remove(old)
    new_me.name = obj.name


def smooth_by_angle(obj, angle_deg=30.0):
    me = obj.data
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(angle_deg))


def select_only(objs):
    bpy.context.view_layer.update()
    for o in bpy.context.scene.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def _select_faces(bm, me, mi):
    """Select exactly the faces of material index mi (None = all) in face-select mode."""
    bm.select_mode = {"FACE"}
    for f in bm.faces:
        f.select_set(False)
    for f in bm.faces:
        if mi is None or f.material_index == mi:
            f.select_set(True)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(me)


def _pack_selected(rotate=True):
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.pack_islands(rotate=rotate, rotate_method="ANY", scale=True,
                            margin_method="FRACTION", margin=UV_PACK_MARGIN,
                            shape_method="CONCAVE")


def unwrap(obj, uv_mode=None, check=None):
    """Unwrap + pack. per-material: each material index gets its own full 0-1 space.
    Returns the per-material uv report of the accepted layout. Objects flagged
    'patina_uv_done' (unwrapped by their builder before beveling) are only re-checked."""
    uv_mode = OPTS["uv_mode"] if uv_mode is None else uv_mode
    check = OPTS["check"] if check is None else check
    if obj.get("patina_skip_uv"):
        return {"_method": "none (high poly)"}
    if obj.get("patina_uv_done"):
        report = uv_report(obj, check)
        report["_method"] = obj["patina_uv_done"]
        return report
    me = obj.data
    if not me.uv_layers:
        me.uv_layers.new(name="UVMap")
    analytic = bool(obj.get("patina_analytic_uv"))     # UVs already written by the builder
    seamed = bool(obj.get("patina_seamed_uv"))         # builder marked seams: plain unwrap
    groups = (sorted({p.material_index for p in me.polygons}) if uv_mode == "per-material"
              else [None])
    orig_seams = [False] * len(me.edges)
    me.edges.foreach_get("use_seam", orig_seams)
    select_only([obj])
    # Blender 5.x defaults to UV sync selection, where uv.select_all() selects the whole mesh
    # and pack_islands() would pack every material together. Turn it off so the UV ops only
    # see the faces selected for the current material.
    bpy.context.scene.tool_settings.use_uv_select_sync = False

    angles = [None] if (analytic or seamed) else (UV_ANGLES if check else UV_ANGLES[:1])
    report = None
    for angle in angles:
        me.edges.foreach_set("use_seam", orig_seams)
        bpy.ops.object.mode_set(mode="EDIT")
        try:
            bm = bmesh.from_edit_mesh(me)
            for mi in groups:
                _select_faces(bm, me, mi)
                if seamed:
                    bpy.ops.uv.select_all(action="SELECT")
                    bpy.ops.uv.unwrap(method="ANGLE_BASED", fill_holes=True, correct_aspect=True,
                                      margin_method="FRACTION", margin=UV_PACK_MARGIN)
                elif not analytic:
                    bpy.ops.uv.smart_project(angle_limit=math.radians(angle),
                                             island_margin=UV_ISLAND_MARGIN, area_weight=0.0,
                                             correct_aspect=True, scale_to_bounds=False)
                    bpy.ops.uv.select_all(action="SELECT")
                    bpy.ops.uv.seams_from_islands(mark_seams=True, mark_sharp=False)
                    for e in bm.edges:                 # keep the authored seams as well
                        if orig_seams[e.index]:
                            e.seam = True
                    bmesh.update_edit_mesh(me)
                    bpy.ops.uv.unwrap(method="MINIMUM_STRETCH", fill_holes=True,
                                      correct_aspect=True, margin_method="FRACTION",
                                      margin=UV_PACK_MARGIN, iterations=10)
                _pack_selected(rotate=not obj.get("patina_uv_no_rotate"))
            _select_faces(bm, me, -1)          # deselect everything
        finally:
            bpy.ops.object.mode_set(mode="OBJECT")
        report = uv_report(obj, check)
        report["_method"] = ("analytic" if analytic else "seams+abf" if seamed
                             else "smart%d+min_stretch" % angle)
        if not check or all(r.get("overlap_px", 0) == 0 for k, r in report.items()
                            if not k.startswith("_")):
            break
        print("  %s: UV overlap at angle %s, retrying with a lower angle" % (obj.name, angle))
    return report


def uv_report(obj, do_raster, res=512):
    """Per material: UV bounds, coverage and (optionally) rasterised overlap fraction."""
    import numpy as np
    me = obj.data
    me.calc_loop_triangles()
    uv = me.uv_layers.active.data
    per_mat, area3d = {}, {}
    for lt in me.loop_triangles:
        per_mat.setdefault(lt.material_index, []).append([tuple(uv[li].uv) for li in lt.loops])
        area3d.setdefault(lt.material_index, []).append(lt.area)
    out = {}
    for mi, tris in sorted(per_mat.items()):
        t = np.asarray(tris, dtype=np.float64)            # (n,3,2)
        lo, hi = t.reshape(-1, 2).min(0), t.reshape(-1, 2).max(0)
        e1, e2 = t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]
        uva = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0])
        a3 = np.asarray(area3d[mi])
        area = uva.sum()
        # texel density uniformity: sqrt(uv area / 3d area) relative to the material average,
        # 5th..95th percentile weighted by surface area (1.0 = perfectly uniform)
        ok = a3 > 1e-12
        dens = np.sqrt((uva[ok] / a3[ok]) / (area / a3[ok].sum()))
        order = np.argsort(dens)
        cw = np.cumsum(a3[ok][order]) / a3[ok].sum()
        p5, p95 = dens[order][np.searchsorted(cw, 0.05)], dens[order][min(np.searchsorted(cw, 0.95), len(cw) - 1)]
        name = me.materials[mi].name if mi < len(me.materials) else str(mi)
        rep = {"tris": len(tris), "uv_min": [round(float(x), 4) for x in lo],
               "uv_max": [round(float(x), 4) for x in hi], "coverage": round(float(area), 3),
               "texel_density_p5_p95": [round(float(p5), 2), round(float(p95), 2)]}
        if do_raster:
            cnt = np.zeros((res, res), np.uint8)
            p = t * res - 0.5                              # pixel-centre space
            eps = 1e-4
            for a, b, c in p:
                x0 = max(int(math.floor(min(a[0], b[0], c[0]))), 0)
                x1 = min(int(math.ceil(max(a[0], b[0], c[0]))), res - 1)
                y0 = max(int(math.floor(min(a[1], b[1], c[1]))), 0)
                y1 = min(int(math.ceil(max(a[1], b[1], c[1]))), res - 1)
                if x1 < x0 or y1 < y0:
                    continue
                d = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
                if abs(d) < 1e-12:
                    continue
                xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
                w0 = ((b[0] - xs) * (c[1] - ys) - (b[1] - ys) * (c[0] - xs)) / d
                w1 = ((c[0] - xs) * (a[1] - ys) - (c[1] - ys) * (a[0] - xs)) / d
                w2 = 1.0 - w0 - w1
                inside = (w0 > eps) & (w1 > eps) & (w2 > eps)
                cnt[y0:y1 + 1, x0:x1 + 1] += inside.astype(np.uint8)
            covered = int((cnt > 0).sum())
            rep["overlap_px"] = int((cnt > 1).sum())
            rep["overlap_frac"] = round(rep["overlap_px"] / max(covered, 1), 5)
        out[name] = rep
    return out


def glb_info(path):
    """Parse the GLB JSON chunk: triangle count, material names, primitive attributes."""
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, _length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        raise RuntimeError("%s is not a GLB file" % path)
    clen, ctype = struct.unpack_from("<II", data, 12)
    if ctype != 0x4E4F534A:
        raise RuntimeError("%s: first chunk is not JSON" % path)
    g = json.loads(data[20:20 + clen])
    tris, attrs = 0, set()
    for mesh in g.get("meshes", []):
        for prim in mesh["primitives"]:
            attrs.update(prim["attributes"].keys())
            if "indices" in prim:
                tris += g["accessors"][prim["indices"]]["count"] // 3
            else:
                tris += g["accessors"][prim["attributes"]["POSITION"]]["count"] // 3
    return {"triangles": tris, "materials": [m.get("name", "?") for m in g.get("materials", [])],
            "nodes": [n.get("name", "?") for n in g.get("nodes", []) if "mesh" in n],
            "attributes": sorted(attrs), "bytes": len(data)}


def export_glb(path):
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=False, export_apply=True,
        export_yup=True, export_normals=True, export_texcoords=True, export_tangents=False,
        export_materials="EXPORT", export_cameras=False, export_lights=False,
        export_animations=False, export_extras=False, export_image_format="NONE")


# --------------------------------------------------------------------------------------
# lathe helper (barrel, hammer handle) with analytic UVs
# --------------------------------------------------------------------------------------

def revolve(bm, profile, segments, u_splits=1, turn_split_deg=45.0, merge_len=0.03,
            max_len=None):
    """profile: list of (radius, z, material_index of the segment to the next point); radius 0
    makes a pole (triangle fan). Also writes analytic UVs (world-unit scale, packed later):
      - side strips: u = arc length around the axis, v = length along the profile;
        a new island starts on a material change or a turn > turn_split_deg between two
        segments that are both >= merge_len long (short steps stay attached);
      - pole fans (lids): planar XY projection, one island each;
      - the circumference is cut into u_splits islands and strips longer than max_len are cut
        at ring boundaries, to keep islands compact (better packing)."""
    uvl = bm.loops.layers.uv.get("UVMap") or bm.loops.layers.uv.new("UVMap")
    rings = []
    for r, z, _mi in profile:
        if r <= 1e-9:
            rings.append([bm.verts.new((0.0, 0.0, z))])
        else:
            rings.append([bm.verts.new((r * math.cos(2 * math.pi * i / segments),
                                        r * math.sin(2 * math.pi * i / segments), z))
                          for i in range(segments)])
    nseg = len(profile) - 1
    seglen, dirs = [], []
    for k in range(nseg):
        dr, dz = profile[k + 1][0] - profile[k][0], profile[k + 1][1] - profile[k][1]
        ln = math.hypot(dr, dz)
        seglen.append(ln)
        dirs.append((dr / ln, dz / ln))

    def is_fan(k):
        return len(rings[k]) == 1 or len(rings[k + 1]) == 1

    v_at = {}        # (segment k, ring index kk) -> v
    v = 0.0
    for k in range(nseg):
        new = k == 0 or is_fan(k) or is_fan(k - 1) or profile[k][2] != profile[k - 1][2]
        if not new:
            cosang = dirs[k][0] * dirs[k - 1][0] + dirs[k][1] * dirs[k - 1][1]
            turn = math.degrees(math.acos(max(-1.0, min(1.0, cosang))))
            new = turn > turn_split_deg and seglen[k] >= merge_len and seglen[k - 1] >= merge_len
        if not new and max_len and v > 0.0 and v + seglen[k] > max_len:
            new = True
        if new:
            v = 0.0
        v_at[(k, k)] = v
        v += seglen[k]
        v_at[(k, k + 1)] = v

    sps = segments // u_splits
    for k in range(nseg):
        a, b, mi = rings[k], rings[k + 1], profile[k][2]
        for i in range(segments):
            j = (i + 1) % segments
            if len(a) == 1:
                f = bm.faces.new((a[0], b[i], b[j]))
            elif len(b) == 1:
                f = bm.faces.new((a[i], b[0], a[j]))
            else:
                f = bm.faces.new((a[i], b[i], b[j], a[j]))
            f.material_index = mi
            if is_fan(k):
                for loop in f.loops:
                    loop[uvl].uv = (loop.vert.co.x, loop.vert.co.y)
                continue
            s = i // sps
            for loop in f.loops:
                kk = k if loop.vert in a else k + 1
                ring = rings[kk]
                ii = ring.index(loop.vert) if len(ring) > 1 else 0
                if ii == 0 and i == segments - 1:
                    ii = segments
                r = profile[kk][0]
                loop[uvl].uv = ((ii - s * sps) / segments * 2 * math.pi * r, v_at[(k, kk)])
    return rings


# --------------------------------------------------------------------------------------
# assets
# --------------------------------------------------------------------------------------

def build_crate():
    mat = make_material("Crate", (0.45, 0.30, 0.16), 0.0, 0.7)
    RIM, RECESS, FRAME, PLATE = 0.075, 0.035, 0.06, 0.02
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    faces = bm.faces[:]
    # outer frame rim, then a recessed panel (concave edges) holding a raised plate (convex)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=RIM, use_even_offset=True)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=0.0, depth=-RECESS)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=FRAME, use_even_offset=True)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=0.0, depth=PLATE)
    obj = object_from_bmesh("Crate", bm, [mat])
    obj.location.z = 0.5                       # sits on the ground
    add_bevel(obj, width=0.006, segments=3)
    apply_modifiers(obj)

    # Seams (marked on the beveled mesh, so the bevels are unwrapped too): between the six
    # sides, and along the diagonals of each side except across the raised plate's top.
    # Every side then unfolds (almost distortion free, it is developable apart from the
    # small bevel corner patches) into one cross-shaped island: plate + 4 arms of
    # plate wall / floor / recess wall / rim. Hollow frame islands would waste UV space.
    plate_h = 0.5 - RECESS + PLATE

    def classify(f):
        c = f.calc_center_median()
        ax = max(range(3), key=lambda i: abs(c[i]))
        sgn = 1 if c[ax] >= 0 else -1
        u, v = [c[i] for i in range(3) if i != ax]
        is_plate = f.normal[ax] * sgn > 0.999 and abs(abs(c[ax]) - plate_h) < 1e-4
        sector = "plate" if is_plate else ((0, u >= 0) if abs(u) >= abs(v) else (1, v >= 0))
        return (ax, sgn), sector

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    cls = {f.index: classify(f) for f in bm.faces}
    for e in bm.edges:
        if len(e.link_faces) != 2:
            continue
        (s1, k1), (s2, k2) = cls[e.link_faces[0].index], cls[e.link_faces[1].index]
        e.seam = s1 != s2 or (k1 != k2 and "plate" not in (k1, k2))
    bm.to_mesh(obj.data)
    bm.free()
    obj["patina_seamed_uv"] = True
    return [obj]


def build_barrel():
    """Chunky, stylized coopered barrel: 14 thick staves with slightly uneven widths and ends, a
    strong bulge, board-built heads set into the stave ends and proud iron hoops, all with big
    rounded bevels. Wood UVs keep the grain along V: every stave face and head board is unfolded
    lengthwise along V and packed without rotation, so UV-space noise ("space": "uv", stretched
    along "up") runs along each board."""
    import random
    rng = random.Random(7)                            # fixed: the asset is reproducible
    staves_m = make_material("Staves", (0.45, 0.29, 0.15), 0.0, 0.6)
    heads_m = make_material("Heads", (0.50, 0.34, 0.18), 0.0, 0.6)
    hoops_m = make_material("Hoops", (0.30, 0.30, 0.31), 1.0, 0.5)
    H, R_END, R_BILGE = 0.85, 0.25, 0.33
    N, T, GAP = 14, 0.034, 0.003                      # staves, thickness, joint gap
    CHIME, HEAD_T, CROZE = 0.045, 0.03, 0.006         # stave ends above the heads, head thickness
    RIVETS = 6                                        # per hoop
    ROWS = 24

    def r_at(z):
        t = (z - H / 2) / (H / 2)
        return R_END + (R_BILGE - R_END) * (1.0 - t * t)

    def across(pts):                                  # arc positions across a section, centred
        acc, out = 0.0, [0.0]
        for (a0, r0), (a1, r1) in zip(pts, pts[1:]):
            acc += math.hypot(r1 * math.cos(a1) - r0 * math.cos(a0), r1 * math.sin(a1) - r0 * math.sin(a0))
            out.append(acc)
        return [x - acc / 2 for x in out]

    # ---- staves: one island per face, lengthwise along v
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")

    def face(verts, uvs):
        f = bm.faces.new(verts)
        for loop, uv in zip(f.loops, uvs):
            loop[uvl].uv = uv
        return f

    widths = [1.0 + rng.uniform(-0.18, 0.18) for _ in range(N)]
    bounds = [0.0]
    for w_ in widths:
        bounds.append(bounds[-1] + w_)
    bounds = [2 * math.pi * b / bounds[-1] for b in bounds]
    half_gap = GAP / R_BILGE / 2
    for i in range(N):
        t0, t1 = bounds[i] + half_gap, bounds[i + 1] - half_gap
        z_lo, z_hi = rng.uniform(-0.004, 0.006), H + rng.uniform(-0.006, 0.004)   # uneven ends
        dr = rng.uniform(-0.003, 0.003)               # staves not quite flush
        zs = [z_lo + (z_hi - z_lo) * k / ROWS for k in range(ROWS + 1)]

        def ro(z):
            return r_at(z) + dr

        def ri(z):
            return r_at(z) + dr - T

        def v_along(r_fn):
            out, acc = [0.0], 0.0
            for a, b in zip(zs, zs[1:]):
                acc += math.hypot(r_fn(b) - r_fn(a), b - a)
                out.append(acc)
            return out

        v_out, v_in = v_along(ro), v_along(ri)
        outer = lambda z: [(t0 + (t1 - t0) * k / 4, ro(z)) for k in range(5)]
        inner = lambda z: [(t0 + (t1 - t0) * k / 3, ri(z)) for k in range(4)]

        def ring(pts, z):
            return [bm.verts.new((r * math.cos(a), r * math.sin(a), z)) for a, r in pts]

        O = [ring(outer(z), z) for z in zs]
        I = [ring(inner(z), z) for z in zs]
        for k in range(ROWS):
            ua, ub = across(outer(zs[k])), across(outer(zs[k + 1]))
            for c in range(4):
                face((O[k][c], O[k][c + 1], O[k + 1][c + 1], O[k + 1][c]),
                     [(ua[c], v_out[k]), (ua[c + 1], v_out[k]), (ub[c + 1], v_out[k + 1]), (ub[c], v_out[k + 1])])
            ia, ib = across(inner(zs[k])), across(inner(zs[k + 1]))
            for c in range(3):                        # mirrored in u so the island is not flipped
                face((I[k][c + 1], I[k][c], I[k + 1][c], I[k + 1][c + 1]),
                     [(-ia[c + 1], v_in[k]), (-ia[c], v_in[k]), (-ib[c], v_in[k + 1]), (-ib[c + 1], v_in[k + 1])])
            face((I[k][0], O[k][0], O[k + 1][0], I[k + 1][0]),          # joint faces (radial)
                 [(0, v_out[k]), (T, v_out[k]), (T, v_out[k + 1]), (0, v_out[k + 1])])
            face((O[k][-1], I[k][-1], I[k + 1][-1], O[k + 1][-1]),
                 [(0, v_out[k]), (T, v_out[k]), (T, v_out[k + 1]), (0, v_out[k + 1])])
        for k, flip in ((0, True), (ROWS, False)):    # end grain
            loop_verts = O[k] + I[k][::-1]
            f = bm.faces.new(loop_verts[::-1] if flip else loop_verts)
            for loop in f.loops:
                co = loop.vert.co
                a = (math.atan2(co.y, co.x) - t0 + math.pi) % (2 * math.pi) - math.pi   # no wrap at +-180 deg
                loop[uvl].uv = (a * R_END, math.hypot(co.x, co.y))
    staves = object_from_bmesh("Staves", bm, [staves_m])
    staves["patina_analytic_uv"] = True
    staves["patina_uv_no_rotate"] = True
    smooth_by_angle(staves, 30.0)
    add_bevel(staves, width=0.006, segments=3)        # big rounded edges on every stave
    apply_modifiers(staves)

    # ---- heads: 4 thick boards side by side across X, grain along Y (= v)
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    board_w = [0.22, 0.28, 0.28, 0.22]                # fractions of the diameter (sum 1)
    for z0 in (CHIME, H - CHIME - HEAD_T):
        R = r_at(z0 + HEAD_T / 2) - T + CROZE         # set into the croze of the staves
        x = -R
        for wf in board_w:
            xa, xb = x + 0.0015, x + wf * 2 * R - 0.0015
            x += wf * 2 * R
            xs = [xa + (xb - xa) * k / 12 for k in range(13)]
            top = [(xx, math.sqrt(max(R * R - xx * xx, 0.0))) for xx in xs]
            outline = top + [(xx, -yy) for xx, yy in reversed(top)]
            lo = [bm.verts.new((xx, yy, z0)) for xx, yy in outline]
            hi = [bm.verts.new((xx, yy, z0 + HEAD_T)) for xx, yy in outline]
            for verts, flip in ((hi, True), (lo, False)):
                f = bm.faces.new(verts[::-1] if flip else verts)
                for loop in f.loops:
                    co = loop.vert.co
                    loop[uvl].uv = ((co.x if flip else -co.x), co.y)
            acc = 0.0
            for k in range(len(outline)):             # board edges
                j = (k + 1) % len(outline)
                seg = math.hypot(outline[j][0] - outline[k][0], outline[j][1] - outline[k][1])
                face((lo[k], lo[j], hi[j], hi[k]), [(acc, 0), (acc + seg, 0), (acc + seg, HEAD_T), (acc, HEAD_T)])
                acc += seg
    heads = object_from_bmesh("Heads", bm, [heads_m])
    heads["patina_analytic_uv"] = True
    heads["patina_uv_no_rotate"] = True
    add_bevel(heads, width=0.006, segments=3)         # rounded board edges
    apply_modifiers(heads)

    # ---- hoops: thick iron bands standing proud of the staves (two at the chimes, two quarter hoops)
    bm = bmesh.new()
    rbm = bmesh.new()                                 # rivets: their own part and texture set
    HT, EPS = 0.009, 0.0015
    for z0, z1 in ((0.05, 0.11), (0.23, 0.28), (H - 0.28, H - 0.23), (H - 0.11, H - 0.05)):
        zz = [z0 + (z1 - z0) * k / 4 for k in range(5)]
        prof = [(r_at(z) + EPS + HT, z, 0) for z in zz] + [(r_at(z) + EPS, z, 0) for z in reversed(zz)]
        prof.append(prof[0])                          # close the band's cross-section
        revolve(bm, prof, 96, u_splits=4)
        # chunky domed rivets around the band, each sitting on the band's outer surface
        zc = (z0 + z1) / 2
        slope = (r_at(zc + 1e-3) - r_at(zc - 1e-3)) / 2e-3
        for k in range(RIVETS):
            a = 2 * math.pi * (k + 0.5 * (int(z0 * 100) % 2)) / RIVETS
            n = Vector((math.cos(a), math.sin(a), -slope)).normalized()
            c = Vector((math.cos(a) * (r_at(zc) + EPS + HT), math.sin(a) * (r_at(zc) + EPS + HT), zc))
            _rivet(rbm, c, n, 0.016, 0.011)
    hoops = object_from_bmesh("Hoops", bm, [hoops_m])
    hoops["patina_analytic_uv"] = True
    bpy.context.view_layer.objects.active = hoops
    bpy.ops.object.select_all(action="DESELECT")
    hoops.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=1e-6)       # weld the closing seam of each band
    bpy.ops.object.mode_set(mode="OBJECT")
    smooth_by_angle(hoops, 30.0)
    add_bevel(hoops, width=0.003, segments=3)         # rounded band edges
    apply_modifiers(hoops)
    rivets = object_from_bmesh("Rivets", rbm, [make_material("Rivets", (0.3, 0.3, 0.31), 1.0, 0.45)])
    rivets["patina_analytic_uv"] = True
    smooth_by_angle(rivets, 40.0)
    return [staves, heads, hoops, rivets]


def _box_bm(sx, sy, sz, cx=0.0, cy=0.0, cz=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(sx, sy, sz), verts=bm.verts)
    bmesh.ops.translate(bm, vec=(cx, cy, cz), verts=bm.verts)
    return bm


def _hex_bolt(bm, x, y, z, r=0.028, h=0.018):
    """Hex bolt head sitting on z, merged into bm."""
    tmp = bmesh.new()
    bmesh.ops.create_cone(tmp, cap_ends=True, cap_tris=False, segments=6, radius1=r, radius2=r, depth=h)
    bmesh.ops.translate(tmp, vec=(x, y, z + h / 2), verts=tmp.verts)
    me = bpy.data.meshes.new("_tmp_bolt")
    tmp.to_mesh(me)
    tmp.free()
    bm.from_mesh(me)
    bpy.data.meshes.remove(me)


PLATE = (1.0, 0.6, 0.1)          # size; sits on z = 0
BAR = (0.6, 0.1, 0.06)           # size; lies on the plate's raised panel
PANEL = 0.012                    # height of the high plate's raised panel


def build_boltplate_low():
    """Bake test, low poly: two hard-edged boxes (flat shaded, split normals). The bar touches the
    plate, so without name matching plate texels next to it would pick up the bar's high poly."""
    plate_m = make_material("Plate", (0.5, 0.5, 0.52), 1.0, 0.4)
    bar_m = make_material("Bar", (0.3, 0.3, 0.32), 1.0, 0.5)
    plate = object_from_bmesh("Plate_low", _box_bm(*PLATE, cz=PLATE[2] / 2), [plate_m])
    bar = object_from_bmesh("Bar_low", _box_bm(*BAR, cz=PLATE[2] + PANEL + BAR[2] / 2), [bar_m])  # on the raised panel
    for o in (plate, bar):
        o.data.shade_flat()
    return [plate, bar]


def build_boltplate_cage():
    """Bake test, explicit cage: the low boxes pushed out along averaged vertex normals. Same triangles
    in the same order as boltplate_low (bake.normal.cage_mesh)."""
    m = make_material("Cage", (0.5, 0.5, 0.5), 0.0, 0.5)
    objs = []
    for name, size, cz in (("Plate_cage", PLATE, PLATE[2] / 2), ("Bar_cage", BAR, PLATE[2] + PANEL + BAR[2] / 2)):
        bm = _box_bm(*size, cz=cz)
        bm.normal_update()
        for v in bm.verts:
            v.co += v.normal * 0.035                # 0.02 above the faces: clears the 0.018 bolt heads
        o = object_from_bmesh(name, bm, [m])
        o.data.shade_flat()
        o["patina_skip_uv"] = True
        objs.append(o)
    return objs


def build_boltplate_high():
    """Bake test, high poly: beveled plate with a raised centre panel and six hex bolts; beveled bar
    with two bolts. Part names match the low file (Plate_high <- Plate_low)."""
    m = make_material("High", (0.5, 0.5, 0.5), 1.0, 0.4)
    bm = _box_bm(*PLATE, cz=PLATE[2] / 2)
    top = [f for f in bm.faces if f.normal.z > 0.9]
    r = bmesh.ops.inset_region(bm, faces=top, thickness=0.09, depth=0.0)
    bmesh.ops.inset_region(bm, faces=top, thickness=0.0, depth=PANEL)      # raised panel
    for x in (-0.44, 0.0, 0.44):
        for y in (-0.255, 0.255):
            _hex_bolt(bm, x, y, PLATE[2])
    plate = object_from_bmesh("Plate_high", bm, [m])
    bm = _box_bm(*BAR, cz=PLATE[2] + PANEL + BAR[2] / 2)
    for x in (-0.24, 0.24):
        _hex_bolt(bm, x, 0.0, PLATE[2] + PANEL + BAR[2], r=0.022, h=0.014)
    bar = object_from_bmesh("Bar_high", bm, [m])
    for o in (plate, bar):
        add_bevel(o, width=0.008, segments=4)
        apply_modifiers(o)
        o["patina_skip_uv"] = True                  # high polys are never textured
    return [plate, bar]


def _rivet(bm, center, normal, r, h, seg=10, rings=4):
    """Domed rivet head on a surface (sunk 1 mm so it never floats), planar UVs in world units."""
    uvl = bm.loops.layers.uv.get("UVMap") or bm.loops.layers.uv.new("UVMap")
    t = normal.orthogonal().normalized()
    b = normal.cross(t)
    ringv = []
    for k in range(rings):
        a = (k / rings) * (math.pi / 2)
        rr, hh = r * math.cos(a), h * math.sin(a) - 0.001
        ringv.append([bm.verts.new(center + (t * math.cos(2 * math.pi * j / seg) + b * math.sin(2 * math.pi * j / seg)) * rr + normal * hh)
                      for j in range(seg)])
    top = bm.verts.new(center + normal * (h - 0.001))
    faces = []
    for k in range(rings - 1):
        for j in range(seg):
            jn = (j + 1) % seg
            faces.append(bm.faces.new((ringv[k][j], ringv[k][jn], ringv[k + 1][jn], ringv[k + 1][j])))
    for j in range(seg):
        faces.append(bm.faces.new((ringv[-1][j], ringv[-1][(j + 1) % seg], top)))
    for f in faces:
        for loop in f.loops:
            d = loop.vert.co - center
            loop[uvl].uv = (d.dot(t), d.dot(b))


def _cyl(bm, p0, p1, r, seg=12):
    """Capped cylinder from p0 to p1."""
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    m = Matrix.Translation((p0 + p1) / 2) @ Vector((0, 0, 1)).rotation_difference(d.normalized()).to_matrix().to_4x4()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r, radius2=r, depth=d.length, matrix=m)


def build_beer_barrel():
    """Beer barrel on its side in a wooden cradle, with a brass tap in the front head.
    materials: Staves, Heads, Hoops, Rivets, Tap, Stand."""
    staves, heads, hoops, rivets = build_barrel()
    H = max(v.co.z for v in staves.data.vertices)
    head_z = max(v.co.z for v in heads.data.vertices)   # outer face of the top head
    LIFT = 0.40                                          # barrel axis height
    # the tap, built in the upright barrel's frame (+Z out of the top head, -Y becomes "down")
    bm = bmesh.new()
    ty = -0.12
    _cyl(bm, (0, ty, head_z - 0.005), (0, ty, head_z + 0.012), 0.036, 12)        # flange
    _cyl(bm, (0, ty, head_z), (0, ty, H + 0.06), 0.02, 12)                       # shank
    _cyl(bm, (0, ty, H + 0.03), (0, ty, H + 0.085), 0.032, 10)                    # body
    _cyl(bm, (0, ty - 0.01, H + 0.058), (0, ty - 0.085, H + 0.058), 0.016, 10)    # spout (down)
    _cyl(bm, (0, ty + 0.01, H + 0.058), (0, ty + 0.1, H + 0.058), 0.011, 8)       # lever (up)
    bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=0.022,
                              matrix=Matrix.Translation((0, ty + 0.11, H + 0.058)))  # knob
    tap = object_from_bmesh("Tap", bm, [make_material("Tap", (0.8, 0.6, 0.3), 1.0, 0.35)])
    add_bevel(tap, width=0.0025, segments=2)
    apply_modifiers(tap)
    # lay everything on its side: +Z -> -Y (front), +Y -> +Z; centre the barrel over the cradle
    rot = Matrix.Rotation(math.pi / 2, 4, "X")
    move = Matrix.Translation((0, H / 2, LIFT))
    for o in (staves, heads, hoops, rivets, tap):
        o.data.transform(move @ rot)
        o.data.update()
    # the holder: two saddles cut to the barrel's curve, joined by two rails
    bm = bmesh.new()
    piece = bm.faces.layers.int.new("piece")        # 1 = saddle, 0 = rail
    W, T_S = 0.37, 0.07
    stave_r = {}
    for v in staves.data.vertices:  # measured barrel radius near each saddle
        key = round(v.co.y, 2)
        stave_r[key] = max(stave_r.get(key, 0.0), math.hypot(v.co.x, v.co.z - LIFT))
    for yc in (-(H / 2 - 0.17), H / 2 - 0.17):
        near = [rv for yk, rv in stave_r.items() if abs(yk - yc) < 0.04]
        r = (max(near) if near else 0.31) + 0.004
        z_top = LIFT - 0.12
        xa = math.sqrt(max(r * r - (LIFT - z_top) ** 2, 0.0))
        outline = [(-W, 0.0), (-0.2, 0.0), (-0.15, 0.06), (0.15, 0.06), (0.2, 0.0), (W, 0.0), (W, z_top), (xa, z_top)]
        a0 = math.atan2(z_top - LIFT, xa)          # right end of the notch (lower right quadrant)
        a1 = math.atan2(z_top - LIFT, -xa)         # left end (lower left quadrant)
        for k in range(1, 16):                     # arc through the bottom of the notch
            a = a0 + ((a1 - a0) % (-2 * math.pi)) * k / 16
            outline.append((r * math.cos(a), LIFT + r * math.sin(a)))
        outline += [(-xa, z_top), (-W, z_top)]
        front = [bm.verts.new((x, yc - T_S / 2, z)) for x, z in outline]
        back = [bm.verts.new((x, yc + T_S / 2, z)) for x, z in outline]
        saddle_faces = [bm.faces.new(front[::-1]), bm.faces.new(back)]
        n = len(outline)
        for k in range(n):
            j = (k + 1) % n
            saddle_faces.append(bm.faces.new((front[k], front[j], back[j], back[k])))
        for f in saddle_faces:
            f[piece] = 1
    for x in (-0.29, 0.29):
        tmp = _box_bm(0.07, H * 0.95, 0.06, cx=x, cz=0.03)
        me = bpy.data.meshes.new("_rail")
        tmp.to_mesh(me)
        tmp.free()
        bm.from_mesh(me)
        bpy.data.meshes.remove(me)
    # UVs in world units (uniform density), projected per face so the grain (V) runs along each
    # plank: saddles along X, rails front to back (Y); packed without rotation
    uvl = bm.loops.layers.uv.new("UVMap")
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.normal_update()                                  # new faces have no normals yet
    for f in bm.faces:
        n = f.normal
        ax = max(range(3), key=lambda i: abs(n[i]))
        for loop in f.loops:
            x, y, z = loop.vert.co
            if f[piece] == 1:   # saddle: length along X
                loop[uvl].uv = (z, x) if ax == 1 else ((y, x) if ax == 2 else (y, z))
            else:               # rail: length along Y
                loop[uvl].uv = (z, y) if ax == 0 else ((x, y) if ax == 2 else (x, z))
    stand = object_from_bmesh("Stand", bm, [make_material("Stand", (0.35, 0.22, 0.12), 0.0, 0.7)])
    stand["patina_analytic_uv"] = True
    stand["patina_uv_no_rotate"] = True
    add_bevel(stand, width=0.008, segments=3)
    apply_modifiers(stand)
    return [staves, heads, hoops, rivets, tap, stand]


def build_hammer():
    wood = make_material("Wood", (0.55, 0.38, 0.20), 0.0, 0.6)
    steel = make_material("Steel", (0.56, 0.57, 0.58), 1.0, 0.35)
    L = 0.34
    prof = [(0.0, 0.0, 0), (0.0165, 0.0, 0), (0.0185, 0.008, 0), (0.0185, 0.05, 0),
            (0.0165, 0.14, 0), (0.0145, 0.24, 0), (0.0150, 0.29, 0), (0.0150, L, 0), (0.0, L, 0)]
    bm = bmesh.new()
    revolve(bm, prof, 32, u_splits=1, max_len=0.12)
    handle = object_from_bmesh("Handle", bm, [wood])
    handle["patina_analytic_uv"] = True
    smooth_by_angle(handle, 35.0)

    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(0.14, 0.036, 0.042), verts=bm.verts[:])
    for v in bm.verts:                         # slight taper toward the claw end (-X)
        if v.co.x < 0:
            v.co.z *= 0.7
    head = object_from_bmesh("Head", bm, [steel])
    head.location = (0.012, 0.0, L - 0.018)
    add_bevel(head, width=0.003, segments=3)
    apply_modifiers(head)
    return [handle, head]


def build_suzanne():
    skin = make_material("Skin", (0.80, 0.55, 0.45), 0.0, 0.5)
    bm = bmesh.new()
    bmesh.ops.create_monkey(bm, matrix=Matrix.Scale(0.5, 4))    # ~1.4 m wide, like the others
    obj = object_from_bmesh("Suzanne", bm, [skin])
    mod = obj.modifiers.new("Subdivision", "SUBSURF")
    mod.levels = 2
    mod.render_levels = 2
    apply_modifiers(obj)
    obj.data.shade_smooth()
    return [obj]


def build_panel():
    mat = make_material("Panel", (0.30, 0.33, 0.36), 1.0, 0.4)
    T = 0.05
    top = T / 2
    # grooves: (kind, (x0, x1, y0, y1), inner rect for rings, depth)
    grooves = [
        ("ring", (-0.42, 0.42, -0.42, 0.42), (-0.39, 0.39, -0.39, 0.39), 0.012),
        ("rect", (-0.30, 0.06, 0.20, 0.235), None, 0.010),
        ("rect", (-0.30, 0.06, 0.12, 0.155), None, 0.010),
        ("rect", (-0.30, -0.265, -0.32, 0.04), None, 0.010),
        ("rect", (0.12, 0.32, -0.32, -0.08), None, 0.008),     # recessed pocket
        ("rect", (0.12, 0.32, 0.08, 0.30), None, 0.006),       # shallow pocket
    ]
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(1.0, 1.0, T), verts=bm.verts[:])
    xs, ys = set(), set()
    for _kind, r, inner, _d in grooves:
        for rr in (r, inner):
            if rr:
                xs.update(rr[:2])
                ys.update(rr[2:])
    for x in sorted(xs):
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                               plane_co=(x, 0, 0), plane_no=(1, 0, 0))
    for y in sorted(ys):
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                               plane_co=(0, y, 0), plane_no=(0, 1, 0))

    def inside(c, r):
        return r[0] < c.x < r[1] and r[2] < c.y < r[3]

    for kind, r, inner, depth in grooves:
        sel = []
        for f in bm.faces:
            c = f.calc_center_median()
            if f.normal.z > 0.99 and abs(c.z - top) < 1e-5 and inside(c, r):
                if kind == "ring" and inside(c, inner):
                    continue
                sel.append(f)
        ret = bmesh.ops.extrude_face_region(bm, geom=sel)
        verts = [e for e in ret["geom"] if isinstance(e, bmesh.types.BMVert)]
        bmesh.ops.translate(bm, verts=verts, vec=(0.0, 0.0, -depth))
        bmesh.ops.delete(bm, geom=sel, context="FACES")        # drop the old top faces

    # bolt heads at the four corners (sunk 1 mm so they read as seated, no bottom cap)
    for bx in (-0.455, 0.455):
        for by in (-0.455, 0.455):
            h = 0.014
            ret = bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=24,
                                        radius1=0.022, radius2=0.022, depth=h,
                                        matrix=Matrix.Translation((bx, by, top - 0.001 + h / 2)))
            vs = set(ret["verts"])
            bottom = [f for f in bm.faces if len(f.verts) > 4 and set(f.verts) <= vs
                      and f.calc_center_median().z < top - 0.0005]
            bmesh.ops.delete(bm, geom=bottom, context="FACES_ONLY")
    obj = object_from_bmesh("Panel", bm, [mat])
    add_bevel(obj, width=0.0025, segments=2)
    apply_modifiers(obj)
    return [obj]


BUILDERS = {"crate": build_crate, "barrel": build_barrel, "beer_barrel": build_beer_barrel, "hammer": build_hammer,
            "boltplate_low": build_boltplate_low, "boltplate_high": build_boltplate_high,
            "boltplate_cage": build_boltplate_cage,
            "suzanne": build_suzanne, "panel": build_panel}


def main():
    args = parse_args()
    names = [n.strip() for n in args.only.split(",") if n.strip()] or list(ASSET_NAMES)
    bad = [n for n in names if n not in BUILDERS]
    if bad:
        raise RuntimeError("unknown asset(s): %s" % ", ".join(bad))
    OPTS.update(uv_mode=args.uv_mode, check=not args.no_check)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    summary = {}
    for name in names:
        reset_scene()
        objs = BUILDERS[name]()
        uv = {o.name: unwrap(o) for o in objs}
        path = os.path.join(out_dir, name + ".glb")
        export_glb(path)
        info = glb_info(path)
        info["uv"] = uv
        summary[name] = info
        if info["triangles"] > 50000:
            print("WARNING: %s has %d triangles (> 50k)" % (name, info["triangles"]))

    print("\n=== Patina test assets (uv-mode %s) -> %s ===" % (args.uv_mode, out_dir))
    for name, s in summary.items():
        print("%-8s %6d tris  %6.1f KB  objects=%s  materials=%s  attrs=%s" % (
            name, s["triangles"], s["bytes"] / 1024.0, ",".join(s["nodes"]),
            ",".join(s["materials"]), ",".join(s["attributes"])))
        for oname, mats in s["uv"].items():
            for mname, r in mats.items():
                if mname.startswith("_"):
                    continue
                ov = (" overlap=%dpx" % r["overlap_px"]) if "overlap_px" in r else ""
                print("         uv %-8s %-12s %-20s tris=%-6d coverage=%.2f density(p5..p95)=%.2f..%.2f%s"
                      % (oname, mname, mats["_method"], r["tris"], r["coverage"],
                         r["texel_density_p5_p95"][0], r["texel_density_p5_p95"][1], ov))
    print("PATINA_JSON " + json.dumps({"ok": True, "out": out_dir, "assets": summary}))


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:
        if e.code not in (0, None):
            sys.exit(e.code if isinstance(e.code, int) else 2)
    except Exception as e:  # noqa: BLE001 - report every failure as a nonzero exit
        traceback.print_exc()
        print("PATINA_JSON " + json.dumps({"ok": False, "error": "%s: %s" % (type(e).__name__, e)}))
        sys.exit(1)
