"""Small file-based CLI smoke test; run in the configured Prism runtime."""
import argparse
import base64
import json
import struct
from pathlib import Path
import subprocess
import tempfile

import OpenImageIO as oiio
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = str(args.executable.resolve())
    help_result = subprocess.run([executable], capture_output=True, text=True, timeout=30)
    assert help_result.returncode == 0 and "--scene" in help_result.stdout
    with tempfile.TemporaryDirectory(prefix="prism cli ") as directory:
        root = Path(directory)
        scene = root / "colored quad.obj"
        (root / "material.mtl").write_text("newmtl diffuse\nKd 0.2 0.6 0.8\nillum 1\n", encoding="utf-8")
        scene.write_text("mtllib material.mtl\nv -.5 -.5 0\nv .5 -.5 0\n"
                         "v .5 .5 0\nv -.5 .5 0\nusemtl diffuse\nf 1 2 3\nf 1 3 4\n", encoding="utf-8")
        output = root / "linear output.exr"
        command = [executable, "--scene", str(scene), "--camera-position", "0,0,4",
                   "--camera-target", "0,0,0", "--env-color", "1,1,1",
                   "--environment-sampling", "bsdf", "--width", "32", "--height", "32",
                   "--spp", "4", "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        pixels = np.asarray(oiio.ImageBuf(str(output)).get_pixels())
        assert np.isfinite(pixels).all()
        assert np.allclose(pixels[16,16,:3], [.2,.6,.8], atol=1e-6), pixels[16,16]
        for filtering in ("filtered", "bilinear"):
            result = subprocess.run(command + ["--texture-filtering", filtering],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            assert np.array_equal(pixels, np.asarray(oiio.ImageBuf(str(output)).get_pixels())), \
                "Filtering changed an untextured deterministic fixture"
        # Exercise both file encodings through the existing glTF importer.
        positions = [-1.,-1.,0., 1.,-1.,0., 0.,1.,0.]
        binary = struct.pack("<24f3H", *(positions + [0.,0.,1.]*3 + [0.,0.,1.,0.,.5,1.]), 0,1,2)
        gltf = dict(asset={"version":"2.0"}, scene=0, scenes=[{"nodes":[0]}],
            nodes=[{"mesh":0}], buffers=[{"byteLength":len(binary)}],
            bufferViews=[{"buffer":0,"byteOffset":offset,"byteLength":size}
                         for offset,size in [(0,36),(36,36),(72,24),(96,6)]],
            accessors=[{"bufferView":index,"componentType":component,"count":count,"type":kind}
                       for index,component,count,kind in [(0,5126,3,"VEC3"),(1,5126,3,"VEC3"),
                                                          (2,5126,3,"VEC2"),(3,5123,3,"SCALAR")]],
            meshes=[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},
                                    "indices":3,"material":0,"mode":4}]}],
            materials=[{"pbrMetallicRoughness":{"baseColorFactor":[.2,.6,.8,1.],
                                                "metallicFactor":0.,"roughnessFactor":1.}}])
        gltf["accessors"][0].update(min=[-1.,-1.,0.], max=[1.,1.,0.])
        for suffix in (".gltf", ".glb"):
            path = root / ("colored triangle"+suffix)
            if suffix == ".gltf":
                gltf["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(binary).decode()
                path.write_text(json.dumps(gltf), encoding="utf-8")
            else:
                del gltf["buffers"][0]["uri"]
                encoded = json.dumps(gltf).encode()
                encoded += b" "*((-len(encoded))%4)
                padded = binary+b"\0"*((-len(binary))%4)
                path.write_bytes(struct.pack("<III",0x46546c67,2,28+len(encoded)+len(padded))+
                    struct.pack("<II",len(encoded),0x4e4f534a)+encoded+
                    struct.pack("<II",len(padded),0x004e4942)+padded)
            gltf_command = command.copy()
            gltf_command[2] = str(path)
            result = subprocess.run(gltf_command, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, result.stdout+result.stderr
            pixels = np.asarray(oiio.ImageBuf(str(output)).get_pixels())
            assert np.isfinite(pixels).all() and (pixels[16,16,:3] > 0).any()
        for invalid in ([executable, "--scene", str(scene)],
                        command + ["--camera", "/World/Camera"],
                        command + ["--env", "conflicting.exr"],
                        command + ["--texture-filtering", "unsupported"]):
            result = subprocess.run(invalid, capture_output=True, text=True, timeout=30)
            assert result.returncode != 0, "Conflicting/missing camera or environment was accepted"
    print("CLI help, explicit camera, OBJ triangles/RGB, glTF/GLB, spaced paths and conflict checks passed")


if __name__ == "__main__":
    main()
