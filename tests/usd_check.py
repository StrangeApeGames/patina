#!/usr/bin/env python3
"""Checks USD files written by `patina export --usd`.

usage: usd_check.py [--expect MESHES,TRIANGLES,MATERIALS,TEXTURES] file.usda|file.usdc|file.usdz ...

Plain Python checks what it can see on its own: the text of a .usda, the header of a .usdc, the zip layout of a
.usdz. With OpenUSD (the pxr module) it also opens every file with the reference implementation, follows each
material to its images and runs OpenUSD's compliance checker. Blender bundles pxr, so when this Python has no pxr
module and a Blender is installed (PATINA_BLENDER, PATH or the usual install folders), the script runs itself
inside Blender for that part. With neither, the OpenUSD part is skipped and says so.

Exit code 0 when every check passed.
"""
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

OPENUSD_OK = "usd_check openusd: ok"
problems = []


def problem(path, text):
    problems.append("%s: %s" % (os.path.basename(path), text))


# ---------------------------------------------------------------- plain Python
def array_length(text):
    text = text.strip()
    return 0 if not text else text.count(",") + 1


def check_usda(path, expect):
    text = open(path, encoding="utf-8").read()
    if not text.startswith("#usda 1.0"):
        problem(path, "does not start with #usda 1.0")
    for needed in ('upAxis = "Y"', "metersPerUnit = 1", "defaultPrim = "):
        if needed not in text:
            problem(path, "layer metadata has no %s" % needed)
    meshes = len(re.findall(r'^\s*def Mesh "', text, re.M))
    # without subdivisionScheme = "none" USD subdivides the mesh and ignores its normals
    if len(re.findall(r'uniform token subdivisionScheme = "none"', text)) != meshes:
        problem(path, 'not every Mesh has subdivisionScheme = "none"')
    triangles = sum(array_length(a) for a in re.findall(r"int\[\] faceVertexCounts = \[([^\]]*)\]", text))
    materials = len(re.findall(r'^\s*def Material "', text, re.M))
    textures = len(re.findall(r'info:id = "UsdUVTexture"', text))
    if re.search(r"texCoord2f inputs:st", text):
        problem(path, "a texture's inputs:st is texCoord2f; the UsdUVTexture spec says float2")
    files = re.findall(r"asset inputs:file = @([^@]*)@", text)
    if len(files) != textures:
        problem(path, "%d texture nodes but %d inputs:file" % (textures, len(files)))
    for f in files:
        if not os.path.isfile(os.path.join(os.path.dirname(path), f)):
            problem(path, "texture %s is not next to the file" % f)
    if expect and (meshes, triangles, materials, textures) != expect:
        problem(path, "expected %s meshes, triangles, materials, textures; the text has %s" % (expect, (meshes, triangles, materials, textures)))


def check_usdc_bytes(path, data):
    if data[:8] != b"PXR-USDC":
        problem(path, "is not a crate file (no PXR-USDC header)")


def check_usdz(path):
    """A .usdz is a zip with nothing compressed, every file's data on a 64-byte boundary and the root layer first."""
    data = open(path, "rb").read()
    try:
        check_usdz_entries(path, data)
    except zipfile.BadZipFile as e:  # not a zip, or a file inside fails its checksum
        problem(path, "is a damaged archive: %s" % e)


def check_usdz_entries(path, data):
    with zipfile.ZipFile(path) as z:
        infos = z.infolist()
        if not infos:
            return problem(path, "is an empty archive")
        if not infos[0].filename.endswith((".usdc", ".usda", ".usd")):
            problem(path, "the first file is %s, not the root layer" % infos[0].filename)
        for info in infos:
            if info.compress_type != zipfile.ZIP_STORED:
                problem(path, "%s is compressed" % info.filename)
            name_len, extra_len = struct.unpack_from("<HH", data, info.header_offset + 26)
            if (info.header_offset + 30 + name_len + extra_len) % 64:
                problem(path, "%s does not start on a 64-byte boundary" % info.filename)
            entry = z.read(info)
            if info.filename.endswith(".usdc"):
                check_usdc_bytes(path, entry)
            elif info.filename.endswith(".png"):
                # the same image the export wrote beside the package
                beside = os.path.join(os.path.dirname(path), info.filename)
                if not os.path.isfile(beside) or open(beside, "rb").read() != entry:
                    problem(path, "%s inside differs from the exported file" % info.filename)
        if not any(i.filename.endswith(".png") for i in infos):
            problem(path, "has no images inside")


def check_plain(path, expect):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".usda":
        check_usda(path, expect)
    elif ext == ".usdc":
        check_usdc_bytes(path, open(path, "rb").read(8))
    elif ext == ".usdz":
        check_usdz(path)
    else:
        problem(path, "is not a .usda, .usdc or .usdz file")


# ---------------------------------------------------------------- OpenUSD
def check_openusd(path, expect):
    from pxr import Usd, UsdGeom, UsdShade, UsdUtils

    stage = Usd.Stage.Open(path)
    if not stage:
        return problem(path, "OpenUSD cannot open it")
    if not stage.GetDefaultPrim():
        problem(path, "has no default prim")
    if UsdGeom.GetStageUpAxis(stage) != "Y" or UsdGeom.GetStageMetersPerUnit(stage) != 1.0:
        problem(path, "is not Y up in meters")
    meshes = triangles = materials = 0
    images = set()
    for prim in stage.Traverse():
        if prim.IsA(UsdGeom.Mesh):
            mesh = UsdGeom.Mesh(prim)
            counts = mesh.GetFaceVertexCountsAttr().Get() or []
            indices = mesh.GetFaceVertexIndicesAttr().Get() or []
            points = mesh.GetPointsAttr().Get() or []
            normals = mesh.GetNormalsAttr().Get() or []
            st = UsdGeom.PrimvarsAPI(prim).GetPrimvar("st")
            uvs = st.Get() if st else None
            meshes += 1
            triangles += len(counts)
            where = str(prim.GetPath())
            if any(c != 3 for c in counts) or len(indices) != 3 * len(counts) or (indices and max(indices) >= len(points)):
                problem(path, "%s: faces do not index its points as triangles" % where)
            if len(normals) != len(points) or mesh.GetNormalsInterpolation() != "vertex":
                problem(path, "%s: normals are not one per point" % where)
            if uvs is None or len(uvs) != len(points) or st.GetInterpolation() != "vertex":
                problem(path, "%s: primvars:st is not one per point" % where)
            if mesh.GetSubdivisionSchemeAttr().Get() != "none":
                problem(path, "%s: is a subdivision surface" % where)
            subsets = UsdGeom.Subset.GetGeomSubsets(mesh, UsdGeom.Tokens.face, "materialBind")
            if subsets:
                # each face in exactly one subset, each subset bound
                valid, why = UsdGeom.Subset.ValidateFamily(mesh, UsdGeom.Tokens.face, "materialBind")
                if not valid or UsdGeom.Subset.GetFamilyType(mesh, "materialBind") != "partition":
                    problem(path, "%s: material subsets are not a partition of its faces %s" % (where, why))
                for subset in subsets:
                    if not UsdShade.MaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial()[0]:
                        problem(path, "%s: subset %s has no material" % (where, subset.GetPrim().GetName()))
            elif not UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]:
                problem(path, "%s: has no material" % where)
        elif prim.IsA(UsdShade.Material):
            materials += 1
            where = str(prim.GetPath())
            source = UsdShade.Material(prim).ComputeSurfaceSource()
            shader = source[0] if source else None
            if not shader or shader.GetIdAttr().Get() != "UsdPreviewSurface":
                problem(path, "%s: no UsdPreviewSurface is connected to its surface" % where)
                continue
            for needed in ("diffuseColor", "roughness", "metallic", "normal"):
                if not shader.GetInput(needed) or not shader.GetInput(needed).GetConnectedSources()[0]:
                    problem(path, "%s: %s reads no texture" % (where, needed))
            for shader_input in shader.GetInputs():
                for connection in shader_input.GetConnectedSources()[0]:
                    texture = UsdShade.Shader(connection.source.GetPrim())
                    name = "%s <- %s" % (shader_input.GetBaseName(), texture.GetPrim().GetName())
                    file_input = texture.GetInput("file")
                    asset = file_input.Get() if file_input else None
                    # inside a .usdz the resolved path is package.usdz[image.png]
                    resolved = asset.resolvedPath if asset else ""
                    if not resolved or not ("[" in resolved or os.path.isfile(resolved)):
                        problem(path, "%s: %s: its image does not resolve (%s)" % (where, name, asset.path if asset else None))
                    images.add(str(texture.GetPrim().GetPath()))
                    st_input = texture.GetInput("st")
                    readers = st_input.GetConnectedSources()[0] if st_input else []
                    reader = UsdShade.Shader(readers[0].source.GetPrim()) if readers else None
                    if not reader or reader.GetIdAttr().Get() != "UsdPrimvarReader_float2" or reader.GetInput("varname").Get() != "st":
                        problem(path, "%s: %s: does not read the st primvar" % (where, name))
    found = (meshes, triangles, materials, len(images))
    if expect and found != expect:
        problem(path, "expected %s meshes, triangles, materials, textures; OpenUSD reads %s" % (expect, found))
    checker = UsdUtils.ComplianceChecker(arkit=False, skipARKitRootLayerCheck=True, rootPackageOnly=False, skipVariants=False, verbose=False)
    checker.CheckCompliance(path)
    for finding in list(checker.GetErrors()) + list(checker.GetFailedChecks()) + list(checker.GetWarnings()):
        problem(path, "usdchecker: %s" % finding)


def find_blender():
    candidates = [os.environ.get("PATINA_BLENDER", ""), shutil.which("blender") or ""]
    candidates += sorted(glob.glob(r"C:\Program Files\Blender Foundation\Blender *\blender.exe"), reverse=True)
    candidates.append("/Applications/Blender.app/Contents/MacOS/Blender")
    return next((c for c in candidates if c and os.path.isfile(c)), None)


def main():
    in_blender = "--" in sys.argv  # blender ... --python usd_check.py -- <our arguments>
    args = sys.argv[sys.argv.index("--") + 1:] if in_blender else sys.argv[1:]
    expect = None
    if args and args[0] == "--expect":
        expect = tuple(int(n) for n in args[1].split(","))
        args = args[2:]
    if not args:
        sys.exit(__doc__)

    try:
        import pxr  # noqa: F401
        have_openusd = True
    except ImportError:
        have_openusd = False

    if in_blender:  # the parent already ran the plain checks
        for path in args:
            check_openusd(path, expect)
        for p in problems:
            print("usd_check problem:", p)
        if not problems:
            print(OPENUSD_OK)
        return

    for path in args:
        check_plain(path, expect)
    how = "layout only"
    if problems:
        pass  # a file that is already known to be broken is not worth opening
    elif have_openusd:
        for path in args:
            check_openusd(path, expect)
        how = "OpenUSD"
    elif find_blender():
        # sys.exit inside Blender does not reliably become its exit code, so the result is read from what it prints
        command = [find_blender(), "-b", "--factory-startup", "--python", os.path.abspath(__file__), "--"]
        command += (["--expect", ",".join(map(str, expect))] if expect else []) + [os.path.abspath(p) for p in args]
        out = subprocess.run(command, capture_output=True, text=True).stdout
        found = [line[len("usd_check problem: "):] for line in out.splitlines() if line.startswith("usd_check problem: ")]
        problems.extend(found)
        if not found and OPENUSD_OK not in out:
            problems.append("OpenUSD check did not finish inside Blender:\n" + out[-2000:])
        how = "OpenUSD in Blender"
    else:
        print("usd_check: no pxr module and no Blender, so OpenUSD did not read these files")

    for p in problems:
        print("usd_check problem:", p)
    if problems:
        sys.exit(1)
    print("usd_check: %d file(s) ok (%s)" % (len(args), how))


main()
