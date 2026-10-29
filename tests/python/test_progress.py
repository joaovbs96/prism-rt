"""Progress callbacks and cancellation (CPU): a False from the callback,
cancel() from another thread, exceptions in callbacks, Ctrl-C
(KeyboardInterrupt) during a blocking render, progressive sessions, and
cancelled commits. CTest runs this file; run it repeatedly to look for
flakiness (``ctest -R prism_python_progress --repeat until-fail:20``)."""

from __future__ import annotations

import _thread
import threading
import time

import numpy as np
import pytest

import prismrt as prism
from conftest import cpu_session, quad_scene

LONG = dict(samples=1_000_000, adaptive_threshold=0.0)  # renders that only end by cancelling


@pytest.fixture()
def scene(renderer):
    scene = quad_scene(renderer)
    scene.commit()
    return scene


def test_progress_calls_and_completion(renderer, scene):
    calls = []
    threads = set()

    def progress(info):
        calls.append((info.phase, info.fraction))
        threads.add(threading.get_ident())
        return True

    result = cpu_session(renderer, scene, samples=16).render(progress)
    assert calls and calls[-1][1] == pytest.approx(1.0)
    assert threading.get_ident() not in threads  # a C++ thread
    assert result.mean_samples == pytest.approx(16)


def test_callback_false_cancels(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)
    with pytest.raises(prism.CancelledError):
        session.render(lambda info: info.samples < 1)
    # The session renders again afterwards.
    assert session.render(samples=2).mean_samples == pytest.approx(2)


def test_cancel_from_another_thread(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)
    started = threading.Event()
    timer = threading.Thread(target=lambda: (started.wait(30), time.sleep(0.2), session.cancel()))
    timer.start()
    try:
        with pytest.raises(prism.CancelledError):
            session.render(lambda info: started.set() or True)
    finally:
        timer.join()


def test_callback_exception_propagates(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)

    def progress(info):
        raise ValueError("stop here")

    with pytest.raises(ValueError, match="stop here"):
        session.render(progress)


def test_keyboard_interrupt_cancels_render(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)
    started = threading.Event()
    timer = threading.Thread(target=lambda: (started.wait(30), time.sleep(0.2), _thread.interrupt_main()))
    timer.start()
    begin = time.monotonic()
    try:
        with pytest.raises(KeyboardInterrupt):
            session.render(lambda info: started.set() or True)
    finally:
        timer.join()
    assert time.monotonic() - begin < 60
    # Cancelled and joined: the session is idle and renders again.
    assert session.render(samples=1).mean_samples == pytest.approx(1)


def test_progressive_callback_false_and_cancel(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)
    passes = []
    session.start(lambda info: passes.append(info.samples) or len(passes) < 3)
    status = session.wait(120)
    assert status.state == prism.SessionState.Cancelled and len(passes) >= 3

    accumulations = session.status().accumulations
    session.start()  # after a cancel: a new accumulation
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        status = session.status()
        if status.accumulations > accumulations and status.samples >= 2 and not session.aov("color").empty:
            break
        session.wait(0.05)
    preview = np.asarray(session.aov("color"))
    assert preview.shape == (24, 32, 3) and preview.max() > 0
    session.cancel()
    assert session.wait(60).state == prism.SessionState.Cancelled


def test_progressive_callback_exception(renderer, scene):
    session = cpu_session(renderer, scene, **LONG)

    def progress(info):
        raise RuntimeError("callback failed")

    session.start(progress)
    with pytest.raises(RuntimeError, match="callback failed"):
        session.wait(120)
    # Reported once.
    assert session.wait(1).state == prism.SessionState.Cancelled


def test_progressive_pause_resume_and_context(renderer, scene):
    with cpu_session(renderer, scene, **LONG) as session:
        session.start()
        while session.status().samples < 1:
            session.wait(0.05)
        session.pause()
        assert session.wait(60).state == prism.SessionState.Paused
        session.resume()
        assert session.status().state == prism.SessionState.Running
    assert session.status().state == prism.SessionState.Cancelled


def test_cancelled_commit_keeps_edits(renderer):
    scene = quad_scene(renderer)
    with pytest.raises(prism.CancelledError):
        scene.commit(lambda info: False)
    assert scene.revision == 0 and scene.pending_edits
    phases = []
    result = scene.commit(lambda info: phases.append(info.phase) or True)
    assert result.revision == 1 and scene.pending_edits == 0 and "materials" in phases


def test_commit_callback_exception(renderer):
    scene = quad_scene(renderer)
    with pytest.raises(ZeroDivisionError):
        scene.commit(lambda info: 1 / 0)
    assert scene.pending_edits
