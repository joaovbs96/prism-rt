"""A render on the Vulkan device from Python (CTest label gpu, RESOURCE_LOCK
gpu): a small scene with allow_fallback=False, against the CPU's image within
the noise of 64 samples. Skipped without a Vulkan device."""

from __future__ import annotations

import os

import numpy as np
import pytest

import prismrt as prism
from conftest import quad_scene


def test_vulkan_render_matches_cpu():
    if os.environ.get("PRISM_DISABLE_VULKAN"):
        pytest.skip("PRISM_DISABLE_VULKAN is set")
    renderer = prism.Renderer(working_space="lin_rec709")
    if not any(d.kind == prism.DeviceKind.Vulkan for d in renderer.devices()):
        pytest.skip("no Vulkan device")
    scene = quad_scene(renderer)
    scene.commit()
    images = {}
    for device in ("cpu", "vulkan"):
        session = renderer.create_session(scene, device=device, allow_fallback=False, width=32, height=24,
                                          camera="camera", samples=64, adaptive_threshold=0.0)
        assert session.device.kind.name.lower() == device and session.device_note == ""
        images[device] = np.asarray(session.render().aovs["color"]).copy()
        del session
    cpu, gpu = images["cpu"], images["vulkan"]
    assert np.isfinite(gpu).all() and gpu.max() > 0
    assert abs(gpu.mean() - cpu.mean()) < 0.05 * cpu.mean()
