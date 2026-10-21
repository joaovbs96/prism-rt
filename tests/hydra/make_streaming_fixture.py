"""Writes the texture streaming fixture: a 1024 x 1024 texture with detail at
every scale and a scene with a textured plane receding from the camera, so
its lookups ask for many mip levels, the finest ones near the camera.

usage: make_streaming_fixture.py <output directory>
"""
import math
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
size = 1024
rows = []
for y in range(size):
    row = bytearray()
    for x in range(size):
        checker = ((x // 4) ^ (y // 4)) & 1
        ring = 0.5 + 0.5 * math.sin(math.hypot(x - 512, y - 512) * 0.15)
        row += bytes((int(40 + 180 * checker), int(255 * ring), int(255 * x / (size - 1))))
    rows.append(bytes(row))
texture = out / "streaming_texture.ppm"
texture.write_bytes(b"P6\n%d %d\n255\n" % (size, size) + b"".join(rows))

(out / "streaming.usda").write_text('''#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "World"
{
    def Camera "Camera"
    {
        float focalLength = 35
        float horizontalAperture = 20
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0.6, 2)
        float3 xformOp:rotateXYZ = (-25, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }

    def Mesh "Floor" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        point3f[] points = [(-2,0,1.5), (2,0,1.5), (2,0,-6), (-2,0,-6)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        normal3f[] normals = [(0,1,0)] (interpolation = "constant")
        texCoord2f[] primvars:st = [(0,0), (2,0), (2,4), (0,4)] (interpolation = "vertex")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/Mat>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
            float inputs:roughness = 0.6
            token outputs:surface
        }
        def Shader "Texture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @streaming_texture.ppm@
            token inputs:sourceColorSpace = "raw"
            token inputs:wrapS = "repeat"
            token inputs:wrapT = "repeat"
            float2 inputs:st.connect = </World/Mat/Reader.outputs:result>
            float3 outputs:rgb
        }
        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
    }

    def DomeLight "Environment"
    {
        color3f inputs:color = (1, 1, 1)
        float inputs:intensity = 1
    }
}
''')
print("wrote", out)
