"""The built-in material preview (docs/material_preview.md):
prismrt.preview_material matches PrismRT --material bit for bit (CPU, fixed
seed and sample count, no adaptive sampling), and the documents, names and
errors of prismrt.usd.load_material_preview."""

from __future__ import annotations

import os
import subprocess

import numpy as np
import pytest

import prismrt as prism

CLI = os.environ.get("PRISMRT_TEST_CLI")
COMPARE = os.environ.get("PRISMRT_TEST_COMPARE")
MARBLE = "assets/materials/standard_surface_marble_solid.mtlx"
TWO = "tests/hydra/fixtures/material_preview/two_materials.mtlx"


@pytest.fixture(scope="module")
def usd():
    return pytest.importorskip("prismrt.usd")


@pytest.mark.skipif(not CLI or not COMPARE, reason="needs PRISMRT_TEST_CLI and PRISMRT_TEST_COMPARE (CTest)")
def test_preview_matches_cli(usd, source_dir, output_dir):
    width, height, samples, seed = 128, 128, 8, 5
    cli_output = output_dir / "preview_marble_cli.exr"
    python_output = output_dir / "preview_marble_python.exr"
    for path in (cli_output, python_output):
        if path.exists():
            path.unlink()
    command = [CLI, "--material", str(source_dir / MARBLE), "--width", str(width), "--height", str(height),
               "--spp", str(samples), "--adaptive-threshold", "0", "--seed", str(seed), "--device", "cpu",
               "--output", str(cli_output)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "Material: Marble_3D of " in completed.stdout

    renderer = prism.Renderer(disable_vulkan=True)
    result = prism.preview_material(source_dir / MARBLE, width=width, height=height, samples=samples, seed=seed,
                                    adaptive_threshold=0.0, device="cpu", allow_fallback=False, renderer=renderer)
    assert result.write(str(python_output), renderer._native) == []
    compared = subprocess.run([COMPARE, str(cli_output), str(python_output), "--exact"], capture_output=True,
                              text=True, timeout=120)
    assert compared.returncode == 0, compared.stdout + compared.stderr
    pixels = np.asarray(result.aovs["color"])
    assert pixels.shape == (height, width, 3) and np.isfinite(pixels).all() and pixels.max() > 0


def test_preview_scene_is_shipped(usd):
    scene_file = usd.material_preview_scene()
    assert os.path.isfile(scene_file)
    folder = os.path.dirname(scene_file)
    for name in ("LICENCE", "ATTRIBUTION.md", "material_surface_geo/subdiv.usda", "maps/ground.ACEScg.exr"):
        assert os.path.isfile(os.path.join(folder, name)), name


def test_documents_and_names(usd, renderer, source_dir):
    # A path, the asset's camera and the stage's lights.
    scene = renderer.create_scene()
    preview = usd.load_material_preview(scene, source_dir / MARBLE)
    assert preview.material_name == "Marble_3D" and preview.document_materials == ["Marble_3D"]
    assert preview.lights > 0 and preview.renderable_placements > 0
    assert scene["preview_material"].value == preview.material.value
    assert scene["preview_camera"].value == preview.camera.value

    # Several materials: the first unless one is named.
    scene = renderer.create_scene()
    first = usd.load_material_preview(scene, source_dir / TWO)
    assert first.material_name == "red" and first.document_materials == ["red", "blue"]
    scene = renderer.create_scene()
    assert usd.load_material_preview(scene, source_dir / TWO, material_name="blue").material_name == "blue"

    # XML text.
    text = (source_dir / TWO).read_text(encoding="utf-8")
    scene = renderer.create_scene()
    assert usd.load_material_preview(scene, text, material_name="blue").material_name == "blue"


def test_materialx_document(usd, renderer, source_dir):
    mx = pytest.importorskip("MaterialX")
    document = mx.createDocument()
    mx.readFromXmlFile(document, str(source_dir / TWO))
    scene = renderer.create_scene()
    preview = usd.load_material_preview(scene, document, material_name="blue")
    assert preview.material_name == "blue"


def test_errors(usd, renderer, source_dir, tmp_path):
    with pytest.raises(prism.NotFoundError, match="does not exist"):
        usd.load_material_preview(renderer.create_scene(), source_dir / "missing.mtlx")
    with pytest.raises(prism.NotFoundError, match=r"has no material 'green' \(its materials: red, blue\)"):
        usd.load_material_preview(renderer.create_scene(), source_dir / TWO, material_name="green")
    empty = tmp_path / "empty.mtlx"
    empty.write_text('<?xml version="1.0"?>\n<materialx version="1.39" />\n', encoding="utf-8")
    with pytest.raises(prism.InvalidArgumentError, match="has no material"):
        usd.load_material_preview(renderer.create_scene(), empty)
    with pytest.raises(prism.InvalidArgumentError, match="cannot read"):
        usd.load_material_preview(renderer.create_scene(), "<materialx version=")
    with pytest.raises(TypeError):
        usd.load_material_preview(renderer.create_scene(), 42)
