"""The interpreter exits cleanly while sessions run: the atexit hook cancels
and joins them (docs/sdk_design.md 3.8, 6.6). Each case is a fresh Python
process that starts rendering and exits without stopping it."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap

import pytest

PRELUDE = """
import os, sys, time
sys.path.insert(0, os.environ["PRISMRT_TEST_PACKAGE_DIR"])
sys.path.insert(0, os.environ["PRISMRT_TEST_DIR"])
import prismrt as prism
from conftest import cpu_session, quad_scene
renderer = prism.Renderer(working_space="lin_rec709", disable_vulkan=True)
scene = quad_scene(renderer)
scene.commit()
"""

CASES = {
    # A progressive session with a Python callback that keeps being called.
    "progressive_callback": """
session = cpu_session(renderer, scene, samples=1_000_000, adaptive_threshold=0.0)
calls = []
session.start(lambda info: calls.append(info.samples) or True)
while len(calls) < 2:
    time.sleep(0.05)
print("exiting", flush=True)
""",
    # A progressive session without a callback, dropped from Python first.
    "progressive_dropped": """
session = cpu_session(renderer, scene, samples=1_000_000, adaptive_threshold=0.0)
session.start()
time.sleep(0.5)
del session
print("exiting", flush=True)
""",
    # Two sessions of one scene, one paused.
    "two_sessions": """
a = cpu_session(renderer, scene, samples=1_000_000, adaptive_threshold=0.0)
b = cpu_session(renderer, scene, samples=1_000_000, adaptive_threshold=0.0, width=16, height=16)
a.start(lambda info: True)
b.start()
time.sleep(0.5)
b.pause()
print("exiting", flush=True)
""",
    # sys.exit with a non-zero code is kept.
    "exit_code": """
session = cpu_session(renderer, scene, samples=1_000_000, adaptive_threshold=0.0)
session.start(lambda info: True)
time.sleep(0.3)
print("exiting", flush=True)
sys.exit(3)
""",
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_exit_with_running_session(case):
    script = textwrap.dedent(PRELUDE) + textwrap.dedent(CASES[case])
    env = dict(os.environ)
    import prismrt
    env["PRISMRT_TEST_PACKAGE_DIR"] = os.path.dirname(os.path.dirname(prismrt.__file__))
    env["PRISMRT_TEST_DIR"] = os.path.dirname(os.path.abspath(__file__))
    completed = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=180,
                               env=env)
    expected = 3 if case == "exit_code" else 0
    output = completed.stdout + completed.stderr
    assert completed.returncode == expected, output
    assert "exiting" in completed.stdout
    assert "leaked" not in output and "Traceback" not in output, output
