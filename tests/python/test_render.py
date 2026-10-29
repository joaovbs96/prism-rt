"""Python renders match the CLI's bit for bit (CPU, fixed seed and sample
count, no adaptive sampling): the same scene, camera and settings through
prismrt.usd + Session.render and through PrismRT.exe, compared with
prism_compare_images --exact; also the in-memory pixels against the EXR
Python wrote."""

from __future__ import annotations

import os
import subprocess

import numpy as np
import pytest

import prismrt as prism

CLI = os.environ.get("PRISMRT_TEST_CLI")
COMPARE = os.environ.get("PRISMRT_TEST_COMPARE")

CASES = [
    # scene, camera, width, height, samples, seed
    ("assets/scenes/CornellBox.usda", "/World/Camera", 64, 64, 8, 3),
    ("assets/scenes/InOneWeekend.usda", "/World/Camera", 80, 45, 4, 11),
]


@pytest.mark.skipif(not CLI or not COMPARE, reason="needs PRISMRT_TEST_CLI and PRISMRT_TEST_COMPARE (CTest)")
@pytest.mark.parametrize("scene_path,camera,width,height,samples,seed", CASES)
def test_python_matches_cli(source_dir, output_dir, scene_path, camera, width, height, samples, seed):
    usd = pytest.importorskip("prismrt.usd")
    stem = os.path.splitext(os.path.basename(scene_path))[0]
    cli_output = output_dir / f"render_{stem}_cli.exr"
    python_output = output_dir / f"render_{stem}_python.exr"
    for path in (cli_output, python_output):
        if path.exists():
            path.unlink()
    command = [CLI, "--scene", str(source_dir / scene_path), "--camera", camera, "--width", str(width),
               "--height", str(height), "--spp", str(samples), "--adaptive-threshold", "0", "--seed", str(seed),
               "--device", "cpu", "--output", str(cli_output)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
    assert completed.returncode == 0, completed.stdout + completed.stderr

    # The CLI's defaults: renderer options from the environment, usdview's
    # default dome for scenes without lights (both scenes have lights).
    renderer = prism.Renderer(disable_vulkan=True)
    scene = renderer.create_scene()
    loaded = usd.load(scene, source_dir / scene_path, included_purposes=["default"])
    assert loaded.renderable_placements > 0 and camera in loaded.cameras
    scene.commit()
    # The CLI clamps samples at 10 by default (--max-sample-radiance); RenderSettings does not.
    session = renderer.create_session(scene, device="cpu", allow_fallback=False, width=width, height=height,
                                      camera=camera, samples=samples, adaptive_threshold=0.0, seed=seed,
                                      sample_clamp=10.0)
    result = session.render()
    assert session.device.kind == prism.DeviceKind.Cpu
    assert result.write(str(python_output), renderer._native) == []

    compared = subprocess.run([COMPARE, str(cli_output), str(python_output), "--exact"], capture_output=True,
                              text=True, timeout=120)
    assert compared.returncode == 0, compared.stdout + compared.stderr

    # The pixels in memory are the ones written.
    pixels = np.asarray(result.aovs["color"])
    assert pixels.shape == (height, width, 3) and np.isfinite(pixels).all() and pixels.max() > 0
    reread = prism.Image(pixels.copy())
    reread_output = output_dir / f"render_{stem}_reread.exr"
    reread.write(str(reread_output), renderer._native)
    compared = subprocess.run([COMPARE, str(python_output), str(reread_output), "--exact"], capture_output=True,
                              text=True, timeout=120)
    assert compared.returncode == 0, compared.stdout + compared.stderr


def test_render_helper(renderer):
    from conftest import quad_scene
    scene = quad_scene(renderer)
    result = prism.render(scene, "camera", width=16, height=12, device="cpu", samples=4, adaptive_threshold=0.0,
                          aovs=("color", "albedo"))
    assert set(result.aovs) == {"color", "albedo"} and result.aovs["albedo"].shape == (12, 16, 3)
    assert result.mean_samples == pytest.approx(4)
