"""Render the stock-USD native-instance binding fixture and validate sharing."""
import os
import re
import subprocess
import sys
import tempfile

import OpenImageIO as oiio


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_native_instance_bindings.py prism_hydra_render.exe")
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    fixture = os.path.join(root, "tests", "hydra", "fixtures", "native_instance_bindings.usda")
    with tempfile.TemporaryDirectory() as directory:
        output = os.path.join(directory, "bindings.exr")
        run = subprocess.run(
            [os.path.abspath(sys.argv[1]), fixture, "/World/Camera", output, "96", "54", "16",
             "--environment-sampling", "bsdf", "--seed", "7"],
            check=True, text=True, capture_output=True, timeout=600)
        match = re.search(r"Render stat prototypeCount:\s*(\d+)", run.stdout)
        if not match or int(match.group(1)) != 1:
            raise RuntimeError("binding-fork instances did not share one geometry prototype\n" + run.stdout)
        image = oiio.ImageBuf(output)
        pixels = image.get_pixels(oiio.FLOAT, oiio.ROI())
        left = pixels[27, 30, :3]
        right = pixels[27, 66, :3]
        if not (left[0] > left[2] * 2.0 and right[2] > right[0] * 2.0):
            raise RuntimeError(f"instance bindings were not distinct: left={left}, right={right}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
