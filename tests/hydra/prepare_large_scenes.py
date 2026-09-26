#!/usr/bin/env python3
"""Create non-destructive USD wrappers for the larger Hydra validation scenes.

Run with the Python interpreter shipped/configured for the local OpenUSD build:
    python tests/hydra/prepare_large_scenes.py

The output layers reference the source datasets by absolute path. No source
asset is edited or copied. Each wrapper adds a camera framed from the loaded
source stage bounds and a dome light using PrismRT's bundled HDR environment.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CHESS = REPO_ROOT / "assets" / "models" / "OpenChessSet" / "chess_set.usda"
DEFAULT_KITCHEN = (
    REPO_ROOT / "assets" / "models" / "Kitchen_set" / "Kitchen_set_instanced.usd"
)
DEFAULT_ENVIRONMENT = (
    REPO_ROOT / "assets" / "environment" / "IndoorEnvironmentHDRI003_4K-HDR.exr"
)
DEFAULT_OUTPUT_DIR = REPO_ROOT / "build" / "hydra-validation"


def _source_bounds(stage, Usd, UsdGeom):
    """Return a conservative world-space bound, or None for empty stages."""
    root = stage.GetDefaultPrim()
    if not root:
        root = stage.GetPseudoRoot()
    cache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_, UsdGeom.Tokens.render, UsdGeom.Tokens.proxy],
        useExtentsHint=True,
    )
    bound = cache.ComputeWorldBound(root).ComputeAlignedBox()
    if bound.IsEmpty():
        return None
    return bound.GetMin(), bound.GetMax()


def _add_framed_camera(stage, bounds, up_axis, UsdGeom, Gf):
    if bounds is None:
        center = Gf.Vec3d(0.0, 0.0, 0.0)
        radius = 1.0
        print("warning: source stage has no measurable bounds; framing a unit scene", file=sys.stderr)
    else:
        minimum, maximum = bounds
        center = (Gf.Vec3d(minimum) + Gf.Vec3d(maximum)) * 0.5
        radius = max((Gf.Vec3d(maximum) - Gf.Vec3d(minimum)).GetLength() * 0.5, 0.01)

    if str(up_axis).upper() == "Z":
        up = Gf.Vec3d(0.0, 0.0, 1.0)
        view_offset = Gf.Vec3d(0.0, -1.0, 0.55)
    else:
        up = Gf.Vec3d(0.0, 1.0, 0.0)
        view_offset = Gf.Vec3d(0.0, 0.55, 1.0)

    focal_length = 35.0
    horizontal_aperture = 36.0
    vertical_aperture = horizontal_aperture * 9.0 / 16.0
    vertical_fov = 2.0 * math.atan(vertical_aperture / (2.0 * focal_length))
    distance = radius / math.sin(vertical_fov * 0.5) * 1.18
    eye = center + view_offset.GetNormalized() * distance
    camera = UsdGeom.Camera.Define(stage, "/World/ValidationCamera")
    camera.CreateFocalLengthAttr(focal_length)
    camera.CreateHorizontalApertureAttr(horizontal_aperture)
    camera.CreateVerticalApertureAttr(vertical_aperture)
    camera.CreateClippingRangeAttr((max(radius * 0.0005, 0.001), distance + radius * 4.0))
    camera.CreateFStopAttr(0.0)
    camera.CreateFocusDistanceAttr(distance)

    # Gf's look-at matrix maps the camera's local -Z/+Y frame into world space.
    camera_to_world = Gf.Matrix4d().SetLookAt(eye, center, up).GetInverse()
    UsdGeom.Xformable(camera.GetPrim()).AddTransformOp().Set(camera_to_world)


def make_wrapper(source_path: Path, output_path: Path, environment_path: Path):
    try:
        from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux
    except ImportError as exc:
        raise RuntimeError(
            "OpenUSD Python bindings are unavailable. Run this script with the "
            "Python interpreter configured by the OpenUSD installation."
        ) from exc

    source_path = source_path.expanduser().resolve(strict=True)
    environment_path = environment_path.expanduser().resolve(strict=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    source_stage = Usd.Stage.Open(str(source_path), load=Usd.Stage.LoadAll)
    if not source_stage:
        raise RuntimeError(f"OpenUSD could not open source stage: {source_path}")
    source_stage.Load()
    up_axis = UsdGeom.GetStageUpAxis(source_stage)
    bounds = _source_bounds(source_stage, Usd, UsdGeom)

    stage = Usd.Stage.CreateNew(str(output_path))
    stage.SetDefaultPrim(UsdGeom.Xform.Define(stage, "/World").GetPrim())
    UsdGeom.SetStageUpAxis(stage, up_axis)
    UsdGeom.SetStageMetersPerUnit(stage, UsdGeom.GetStageMetersPerUnit(source_stage))

    asset = UsdGeom.Xform.Define(stage, "/World/Asset")
    # Absolute asset paths keep wrappers independent of the caller's cwd.
    asset.GetPrim().GetReferences().AddReference(str(source_path))
    _add_framed_camera(stage, bounds, up_axis, UsdGeom, Gf)

    dome = UsdLux.DomeLight.Define(stage, "/World/ValidationDome")
    dome.CreateTextureFileAttr(Sdf.AssetPath(str(environment_path)))
    dome.CreateTextureFormatAttr(UsdLux.Tokens.latlong)
    dome.CreateColorAttr(Gf.Vec3f(1.0, 1.0, 1.0))
    dome.CreateIntensityAttr(1.0)
    dome.CreateExposureAttr(0.0)

    if not stage.GetRootLayer().Save():
        raise RuntimeError(f"Could not save wrapper stage: {output_path}")
    stage = None
    print(f"created {output_path}")
    print(f"  reference: {source_path}")
    print(f"  camera: /World/ValidationCamera (bounds framed)")
    print(f"  dome: {environment_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chess", type=Path, default=DEFAULT_CHESS,
                        help=f"OpenChessSet USD layer (default: {DEFAULT_CHESS})")
    parser.add_argument("--kitchen", type=Path, default=DEFAULT_KITCHEN,
                        help=f"instanced Kitchen USD layer (default: {DEFAULT_KITCHEN})")
    parser.add_argument("--environment", type=Path, default=DEFAULT_ENVIRONMENT,
                        help=f"HDR environment texture (default: {DEFAULT_ENVIRONMENT})")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR,
                        help=f"wrapper output directory (default: {DEFAULT_OUTPUT_DIR})")
    args = parser.parse_args()

    output_dir = args.output_dir.expanduser().resolve()
    make_wrapper(args.chess, output_dir / "OpenChessSet.usda", args.environment)
    make_wrapper(args.kitchen, output_dir / "Kitchen_instanced.usda", args.environment)


if __name__ == "__main__":
    main()
