"""Image files of render results (prism_io; docs/color_management.md, "Outputs").

EXR files hold every layer as linear floats in the renderer's working space;
other formats (PNG, JPEG, ...) the colour, display-encoded through the
renderer's display and view.
"""

from __future__ import annotations

import os

from . import _prism

__all__ = ["write", "is_multi_channel_format"]

is_multi_channel_format = _prism.is_multi_channel_format


def write(path, value, renderer=None, *, layers=None, multi_part: bool = False, aov=None) -> list[str]:
    """Writes a RenderResult (every AOV, or ``layers``) or one Image (as
    ``aov``) to ``path``; returns the warnings. ``renderer``: the Renderer
    whose colour management applies (default: the one that rendered it)."""
    native = getattr(renderer, "_native", renderer)
    if native is None:
        native = getattr(value, "_renderer", None)
    if isinstance(value, _prism.RenderResult):
        return value.write(os.fspath(path), native, layers, multi_part)
    return value.write(os.fspath(path), native, aov)
