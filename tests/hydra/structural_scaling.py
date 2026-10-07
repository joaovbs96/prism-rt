#!/usr/bin/env python3
"""Generate and run stock-usdview structural scaling diagnostics."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


LEVELS = {
    "assignments": (16, 128, 1024),
    "identical_materials": (1, 16, 64),
    "color_variants": (1, 16, 64),
    "structures": (1, 4, 16),
    "placements": (16, 128, 1024, 16384),
    "partition": (1, 16, 128),
    "textures_alias": (1, 8, 32),
    "textures_unique": (1, 8, 32),
}


def sha256(path):
    digest = hashlib.sha256()
    if path and path.is_file():
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()
    return None


def material(name, color=(0.5, 0.5, 0.5), structure=None, texture=None):
    lines = [f'def Material "{name}" {{',
             f'    token outputs:mtlx:surface.connect = </World/Looks/{name}/Surface.outputs:out>',
             '    def Shader "Surface" {',
             '        uniform token info:id = "ND_standard_surface_surfaceshader"',
             '        float inputs:specular = 0',
             '        float inputs:diffuse_roughness = 0']
    if texture is not None:
        lines += [f'        color3f inputs:base_color.connect = </World/Looks/{name}/Image.outputs:out>',
                  '        token outputs:out', '    }',
                  '    def Shader "Image" {',
                  '        uniform token info:id = "ND_image_color3"',
                  f'        asset inputs:file = @{texture}@',
                  '        color3f inputs:default = (0.5, 0.5, 0.5)',
                  '        token outputs:out', '    }', '}']
        return lines
    if structure is None:
        lines.append(f'        color3f inputs:base_color = ({color[0]:.6f}, {color[1]:.6f}, {color[2]:.6f})')
    else:
        lines.append(f'        color3f inputs:base_color.connect = </World/Looks/{name}/N3.outputs:out>')
    lines += ['        token outputs:out', '    }']
    if structure is not None:
        lines += ['    def Shader "Coordinates" {',
                  '        uniform token info:id = "ND_texcoord_vector2"',
                  '        float2 outputs:out', '    }',
                  '    def Shader "CoordinateColor" {',
                  '        uniform token info:id = "ND_convert_vector2_color3"',
                  f'        float2 inputs:in.connect = </World/Looks/{name}/Coordinates.outputs:out>',
                  '        color3f outputs:out', '    }']
    for index in range(4 if structure is not None else 0):
        node = f"N{index}"
        category = "ND_multiply_color3" if ((structure >> index) & 1) else "ND_add_color3"
        lines += [f'    def Shader "{node}" {{', f'        uniform token info:id = "{category}"']
        if index:
            lines.append(f'        color3f inputs:in1.connect = </World/Looks/{name}/N{index-1}.outputs:out>')
        else:
            lines.append(f'        color3f inputs:in1.connect = </World/Looks/{name}/CoordinateColor.outputs:out>')
        operand = "(0.9, 0.9, 0.9)" if "multiply" in category else "(0.03, 0.03, 0.03)"
        lines += [f'        color3f inputs:in2 = {operand}', '        color3f outputs:out', '    }']
    lines.append('}')
    return lines


def quad(name, index, material_path, size=0.035):
    x = ((index % 32) / 31.0 - 0.5) * 1.8
    y = (((index // 32) % 32) / 31.0 - 0.5) * 1.8
    s = size
    return [f'def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {{',
            f'    point3f[] points = [({-s:.6f}, {-s:.6f}, 0), ({s:.6f}, {-s:.6f}, 0), ({s:.6f}, {s:.6f}, 0), ({-s:.6f}, {s:.6f}, 0)]',
            '    int[] faceVertexCounts = [4]',
            '    int[] faceVertexIndices = [0, 1, 2, 3]',
            '    uniform token subdivisionScheme = "none"',
            '    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")',
            '    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")',
            '    rel material:binding = <' + material_path + '>',
            f'    double3 xformOp:translate = ({x:.6f}, {y:.6f}, 0)',
            '    uniform token[] xformOpOrder = ["xformOp:translate"]', '}']


def triangle_partition(name, start, count, material_path):
    points, indices = [], []
    for local in range(count):
        global_index = start + local
        x = ((global_index % 64) / 63.0 - 0.5) * 1.9
        y = ((global_index // 64) / 63.0 - 0.5) * 1.9
        base = len(points)
        points += [(x, y, 0), (x + .012, y, 0), (x, y + .012, 0)]
        indices += [base, base + 1, base + 2]
    point_text = ", ".join(f"({x:.6f}, {y:.6f}, {z})" for x, y, z in points)
    index_text = ", ".join(map(str, indices))
    return [f'def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {{', f'    point3f[] points = [{point_text}]',
            f'    int[] faceVertexCounts = [{", ".join(["3"] * count)}]',
            f'    int[] faceVertexIndices = [{index_text}]',
            '    uniform token subdivisionScheme = "none"',
            f'    rel material:binding = <{material_path}>',
            '    double3 xformOp:translate = (0, 0, 0)',
            '    uniform token[] xformOpOrder = ["xformOp:translate"]', '}']


def write_ppm(path, variant):
    path.parent.mkdir(parents=True, exist_ok=True)
    row = bytearray()
    for x in range(512):
        for y in range(1):
            checker = ((x // 16) + variant) & 1
            row += bytes(((37 * variant + 211 * checker) & 255,
                          (83 * variant + 127 * (1-checker)) & 255,
                          (19 * variant + 173 * checker) & 255))
    with path.open("wb") as stream:
        stream.write(b"P6\n512 512\n255\n")
        for y in range(512):
            stream.write(row if (y // 16) % 2 == 0 else row[::-1])


def header():
    return ['#usda 1.0', '(', '    defaultPrim = "World"', '    upAxis = "Y"', ')',
            'def Xform "World" {', 'def Camera "Camera" {',
            '    float focalLength = 50', '    float horizontalAperture = 20.955',
            '    float verticalAperture = 20.955',
            '    double3 xformOp:translate = (0, 0, 5)',
            '    uniform token[] xformOpOrder = ["xformOp:translate"]', '}',
            'def Scope "Looks" {']


def generate_case(axis, value, directory):
    directory.mkdir(parents=True, exist_ok=True)
    lines = header()
    meta = {"axis": axis, "value": value}
    edit_material = "/World/Looks/EditMaterial/Surface"
    edit_prim, edit_mesh, edit_instances = "/World/G0", "/World/G0", None

    if axis == "placements":
        lines += material("EditMaterial") + ['}']
        lines += ['def Xform "Prototype" {', 'def Mesh "Mesh" (prepend apiSchemas = ["MaterialBindingAPI"]) {',
                  '    point3f[] points = [(-.02, -.02, 0), (.02, -.02, 0), (.02, .02, 0), (-.02, .02, 0)]',
                  '    int[] faceVertexCounts = [4]', '    int[] faceVertexIndices = [0, 1, 2, 3]',
                  '    uniform token subdivisionScheme = "none"',
                  '    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")',
                  '    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")',
                  '    rel material:binding = </World/Looks/EditMaterial>', '}', '}']
        positions = []
        for i in range(value):
            positions.append(f"({((i%32)/31-.5)*1.8:.6f}, {(((i//32)%32)/31-.5)*1.8:.6f}, 0)")
        lines += ['def PointInstancer "Instances" {', '    rel prototypes = [</World/Prototype>]',
                  f'    point3f[] positions = [{", ".join(positions)}]',
                  f'    int[] protoIndices = [{", ".join(["0"]*value)}]',
                  '    double3 xformOp:translate = (0, 0, 0)',
                  '    uniform token[] xformOpOrder = ["xformOp:translate"]', '}', '}']
        edit_prim, edit_mesh, edit_instances = "/World/Instances", "/World/Prototype/Mesh", "/World/Instances"
        meta.update(materials=1, meshes=1, assignments=value, expectedFanout=value)
    else:
        if axis == "assignments":
            lines += material("EditMaterial")
            lines += material("Palette0")
            mesh_count, palette_count = value, 1
        elif axis in ("identical_materials", "color_variants", "structures"):
            palette_count, mesh_count = value, 128
            for i in range(value):
                color = ((i * 37 % 251) / 250, (i * 73 % 251) / 250, (i * 109 % 251) / 250)
                graph_color = color if axis == "color_variants" else (0.5, 0.5, 0.5)
                graph_structure = i if axis == "structures" else None
                if i == 0:
                    lines += material("EditMaterial", graph_color, graph_structure)
                lines += material(f"Palette{i}", graph_color, graph_structure)
        elif axis.startswith("textures_"):
            palette_count, mesh_count = value, 33
            texture_dir = directory / "textures"
            base = texture_dir / "source.ppm"
            write_ppm(base, 0)
            lines += material("EditMaterial", texture="textures/source.ppm")
            for i in range(value):
                target = texture_dir / f"t{i}.ppm"
                if axis == "textures_alias":
                    shutil.copyfile(base, target)
                else:
                    write_ppm(target, i)
                lines += material(f"Palette{i}", texture=f"textures/t{i}.ppm")
        elif axis == "partition":
            palette_count, mesh_count = 1, value
            lines += material("EditMaterial")
            lines += material("Palette0")
        lines.append('}')
        if axis == "partition":
            total = 4096
            base, extra, first = total // value, total % value, 0
            for i in range(value):
                count = base + (1 if i < extra else 0)
                lines += triangle_partition(f"G{i}", first, count,
                                            "/World/Looks/EditMaterial" if i == 0 else "/World/Looks/Palette0")
                first += count
            meta.update(triangles=4096, meshes=value, materials=2, expectedFanout=1)
        else:
            for i in range(mesh_count):
                binding = "/World/Looks/EditMaterial" if i == 0 else f"/World/Looks/Palette{(i-1)%palette_count}"
                lines += quad(f"G{i}", i, binding)
            meta.update(meshes=mesh_count, materials=palette_count + 1,
                        assignments=mesh_count, expectedFanout=1)
            if axis == "structures":
                meta.update(arithmeticNodesPerGraph=4, auxiliaryNodesPerGraph=2,
                            structuralGraphs=value, dynamicInput="surface UV")
            if axis.startswith("textures_"):
                meta.update(textureFiles=value + 1, textureResolution=[512, 512],
                            textureContents="identical" if axis == "textures_alias" else "unique")
        lines.append('}')

    meta.update(editPrim=edit_prim, editMesh=edit_mesh, editMaterial=edit_material)
    if edit_instances:
        meta["editInstancePositions"] = edit_instances
    scene = directory / f"{axis}-{value}.usda"
    scene.write_text("\n".join(lines) + "\n", encoding="utf-8")
    (directory / "case.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return scene, meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--axis", action="append", choices=LEVELS,
                        help="Axis to run; repeat the option. Default: all axes")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--value", type=int, help="Run only this level of selected axes")
    parser.add_argument("--output", type=Path, default=Path("build/structural-scaling"))
    parser.add_argument("--testusdview", type=Path)
    parser.add_argument("--plugin-binary", type=Path)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    if not args.generate_only and (not args.testusdview or not args.testusdview.is_file()):
        parser.error("--testusdview must name the stock testusdview script")
    root = Path(__file__).resolve().parents[2]
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    callback = Path(__file__).with_name("usdview_structural.py").resolve()
    plugin = args.plugin_binary.resolve() if args.plugin_binary else root / "build/hydra/plugin/hdPrism.dll"
    axes = args.axis or list(LEVELS)
    cases = []
    for axis in axes:
        for value in LEVELS[axis]:
            if args.value is not None and value != args.value:
                continue
            scene, metadata = generate_case(axis, value, output / "cases" / f"{axis}-{value}")
            cases.append((axis, value, scene, metadata))
    report = {"settings": {"axes": axes, "levels": {a: LEVELS[a] for a in axes},
              "repeats": args.repeats, "processModel": "fresh process and empty renderer cache per run",
              "osCache": "not flushed; uncontrolled, potentially warm",
              "materialThreads": 4, "initialRenderer": "HdStormRendererPlugin",
              "testusdview": str(args.testusdview.resolve()) if args.testusdview else None,
              "callback": str(callback), "pluginBinary": str(plugin.resolve()),
              "pluginSha256": sha256(plugin)}, "cases": [c[3] | {"scene": str(c[2])} for c in cases],
              "runs": []}
    report_path = output / "results.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if args.generate_only:
        print(f"Generated {len(cases)} cases in {output / 'cases'}")
        return
    python_path = str(Path(__file__).parent.resolve())
    for axis, value, scene, metadata in cases:
        for repeat in range(1, args.repeats + 1):
            run_dir = output / "runs" / f"{axis}-{value}-run{repeat}"
            cache = run_dir / "renderer-cache"
            validation = run_dir / "validation"
            run_dir.mkdir(parents=True, exist_ok=True)
            cache.mkdir(parents=True, exist_ok=False)
            validation.mkdir(parents=True, exist_ok=True)
            command = [sys.executable, str(args.testusdview.resolve()), str(scene),
                       "--renderer", "HdStormRendererPlugin", "--testScript", str(callback)]
            env = os.environ.copy()
            env.update(PRISM_CACHE_DIR=str(cache), PRISM_MATERIAL_THREADS="4",
                       PRISM_STRUCTURAL_CASE=json.dumps(metadata),
                       PRISM_VALIDATION_DIR=str(validation),
                       PYTHONPATH=python_path + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else ""))
            log = run_dir / "usdview.log"
            started = time.perf_counter()
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.run(command, env=env, stdout=stream,
                                         stderr=subprocess.STDOUT, timeout=900)
            entry = {"axis": axis, "value": value, "repeat": repeat,
                     "command": command, "processElapsedSeconds": time.perf_counter() - started,
                     "exitCode": process.returncode, "scene": str(scene), "case": metadata,
                     "cacheLabel": "fresh-empty-per-run", "cache": str(cache),
                     "validation": str(validation), "log": str(log),
                     "pluginSha256": report["settings"]["pluginSha256"]}
            probe = validation / "probe.json"
            if probe.is_file():
                entry["probe"] = json.loads(probe.read_text(encoding="utf-8"))
            if process.returncode:
                text = log.read_text(encoding="utf-8", errors="replace")
                entry["errorLogTail"] = text[-12000:]
            report["runs"].append(entry)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(json.dumps({k: entry[k] for k in ("axis", "value", "repeat", "exitCode", "processElapsedSeconds")}), flush=True)
            if process.returncode:
                raise RuntimeError(f"structural run failed; see {log}")


if __name__ == "__main__":
    main()
