"""Edit a USD stage in memory with pxr and render it without saving:
change a light, add a sphere, re-render. Useful in pipeline tools and
notebooks that already hold a stage. Import pxr before prismrt and PySide6.

    python 09_usd_unsaved_edits.py assets/scenes/CornellBox.usda --camera /World/Camera
"""

import argparse

from pxr import Gf, Usd, UsdGeom, UsdLux, UsdUtils   # pxr first (DLL load order)

import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--camera", required=True)
parser.add_argument("--size", type=int, default=400)
parser.add_argument("--spp", type=int, default=32)
args = parser.parse_args()

renderer = prism.Renderer()


def render(stage, output):
    scene = renderer.create_scene()
    prism.usd.load(scene, stage)          # unsaved edits render; the caller's stage is not touched
    result = prism.render(scene, args.camera, width=args.size, height=args.size, device="cpu",
                          samples=args.spp, seed=1)
    prism.io.write(output, result.aovs["color"])
    print(f"wrote {output}")


stage = Usd.Stage.Open(args.scene)
render(stage, "usd_before.png")

# Unsaved edits: dim and tint every light, add a red sphere at the origin.
for prim in stage.Traverse():
    if prim.HasAPI(UsdLux.LightAPI):
        light = UsdLux.LightAPI(prim)
        light.GetIntensityAttr().Set(light.GetIntensityAttr().Get() * 0.5)
        light.CreateColorAttr().Set(Gf.Vec3f(1.0, 0.8, 0.6))
# Size and place the sphere from the stage's bounds, whatever its units.
bounds = UsdGeom.BBoxCache(Usd.TimeCode.Default(), ["default", "render"]).ComputeWorldBound(
    stage.GetPseudoRoot()).ComputeAlignedRange()
size = bounds.GetSize()
radius = 0.12 * min(size[0], size[1], size[2])
center = bounds.GetMidpoint()
sphere = UsdGeom.Sphere.Define(stage, "/ExtraSphere")
sphere.GetRadiusAttr().Set(radius)
sphere.GetDisplayColorAttr().Set([Gf.Vec3f(0.8, 0.1, 0.1)])
UsdGeom.XformCommonAPI(sphere).SetTranslate(Gf.Vec3d(center[0], bounds.GetMin()[1] + radius, center[2]))
render(stage, "usd_after.png")

# A stage already in UsdUtils.StageCache (e.g. owned by a DCC session) loads through the cache by id.
with Usd.StageCacheContext(UsdUtils.StageCache.Get()):
    cached = Usd.Stage.Open(args.scene)
stage_id = UsdUtils.StageCache.Get().GetId(cached).ToLongInt()
scene = renderer.create_scene()
prism.usd.load_cached(scene, stage_id)
print("loaded from the stage cache:", scene)
print("edits only in memory, file unchanged on disk:", stage.GetRootLayer().dirty)
