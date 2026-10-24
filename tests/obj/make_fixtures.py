"""Writes the synthetic OBJ/MTL fixtures of tests/obj (docs/obj.md).

python tests/obj/make_fixtures.py [output directory, default: this one]

Small, deterministic files, each viewed from (0, 0, 6) towards the origin:
- textured: four quads with map_Kd (with -s), map_Ks/map_Ns, map_d (the
  colour map's alpha), bump (-bm) and norm;
- glass: a refracting sphere (illum 7, Ni, Tf) and a dissolving one (illum 4);
- emissive: an emitter (Ke) and a textured emitter (map_Ke) over a grey quad;
- pbr: four spheres of the PBR extension (Pr/Pm, Ps, Pc/Pcr, aniso, map_Pr).
"""
import math
import struct
import sys
import zlib
from pathlib import Path


def png(width, height, pixel):
    """RGBA8 PNG bytes; pixel(x, y) -> (r, g, b, a) in 0..255."""
    rows = b"".join(b"\x00" + bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


class Obj:
    def __init__(self, mtl):
        self.lines = ["# PrismRT make_fixtures.py", "mtllib %s" % mtl]
        self.v = self.vt = self.vn = 0

    def quad(self, material, center, size):
        cx, cy, cz = center
        h = size / 2
        self.lines.append("usemtl %s" % material)
        for x, y in ((-h, -h), (h, -h), (h, h), (-h, h)):
            self.lines.append("v %g %g %g" % (cx + x, cy + y, cz))
        for u, v in ((0, 0), (1, 0), (1, 1), (0, 1)):
            self.lines.append("vt %g %g" % (u, v))
        self.lines.append("vn 0 0 1")
        n = self.vn + 1
        b, t = self.v + 1, self.vt + 1
        self.lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d" % (b, t, n, b + 1, t + 1, n, b + 2, t + 2, n, b + 3, t + 3, n))
        self.v += 4
        self.vt += 4
        self.vn += 1

    def sphere(self, material, center, radius, rings=16, segments=32):
        cx, cy, cz = center
        self.lines.append("usemtl %s" % material)
        first = self.v + 1
        for i in range(rings + 1):
            theta = math.pi * i / rings
            for j in range(segments + 1):
                phi = 2 * math.pi * j / segments
                n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
                self.lines.append("v %.6f %.6f %.6f" % (cx + radius * n[0], cy + radius * n[1], cz + radius * n[2]))
                self.lines.append("vt %.6f %.6f" % (j / segments, 1 - i / rings))
                self.lines.append("vn %.6f %.6f %.6f" % n)
        row = segments + 1
        for i in range(rings):
            for j in range(segments):
                a = first + i * row + j
                b, c, d = a + row, a + row + 1, a + 1
                corners = [(a, a), (d, d), (c, c), (b, b)]
                self.lines.append("f " + " ".join("%d/%d/%d" % (v, v - first + self.vt + 1, v - first + self.vn + 1)
                                                  for v, _ in corners))
        count = (rings + 1) * row
        self.v += count
        self.vt += count
        self.vn += count

    def write(self, path):
        path.write_text("\n".join(self.lines) + "\n", newline="\n")


def write_textures(out):
    # sRGB checker (red / white), RGBA leaf mask, height bumps, tangent-space normals.
    (out / "checker.png").write_bytes(png(16, 16, lambda x, y: (200, 30, 30, 255) if (x // 4 + y // 4) % 2 else (240, 240, 240, 255)))

    def leaf(x, y):
        inside = (x - 15.5) ** 2 + (y - 15.5) ** 2 < 12 ** 2
        return (40, 160, 60, 255 if inside else 0)
    (out / "leaf.png").write_bytes(png(32, 32, leaf))
    (out / "height.png").write_bytes(png(32, 32, lambda x, y: (int(127.5 + 127.5 * math.sin(x * math.pi / 4)),) * 3 + (255,)))

    def normal(x, y):
        nx = 0.6 * math.cos(x * math.pi / 4)
        nz = math.sqrt(1 - nx * nx)
        return (int(round((nx * 0.5 + 0.5) * 255)), 128, int(round((nz * 0.5 + 0.5) * 255)), 255)
    (out / "normal.png").write_bytes(png(32, 32, normal))
    (out / "specular.png").write_bytes(png(16, 16, lambda x, y: (255, 255, 255, 255) if x < 8 else (20, 20, 20, 255)))
    (out / "shininess.png").write_bytes(png(16, 16, lambda x, y: (255, 255, 255, 255) if y < 8 else (8, 8, 8, 255)))
    (out / "roughness.png").write_bytes(png(16, 16, lambda x, y: (int(255 * x / 15),) * 3 + (255,)))
    (out / "glow.png").write_bytes(png(16, 16, lambda x, y: (255, 200, 80, 255) if (x // 4 + y // 4) % 2 else (40, 40, 40, 255)))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent
    out.mkdir(parents=True, exist_ok=True)
    write_textures(out)

    textured = Obj("textured.mtl")
    textured.quad("checker", (-1.1, 1.1, 0), 2)
    textured.quad("cutout", (1.1, 1.1, 0), 2)
    textured.quad("bumped", (-1.1, -1.1, 0), 2)
    textured.quad("normal", (1.1, -1.1, 0), 2)
    textured.write(out / "textured.obj")
    (out / "textured.mtl").write_text("""# PrismRT make_fixtures.py
newmtl checker
Ka 0 0 0
Kd 1 1 1
Ks 0.5 0.5 0.5
Ns 200
illum 2
map_Kd -s 2 2 1 checker.png
map_Ks specular.png
map_Ns shininess.png

newmtl cutout
Kd 1 1 1
Ks 0 0 0
illum 2
map_Kd leaf.png
map_d leaf.png

newmtl bumped
Kd 0.8 0.8 0.8
Ks 0.1 0.1 0.1
Ns 20
illum 2
bump -bm 2 height.png

newmtl normal
Kd 0.8 0.8 0.8
Ks 0.1 0.1 0.1
Ns 20
illum 2
norm normal.png
""", newline="\n")

    glass = Obj("glass.mtl")
    glass.sphere("refracting", (-1.2, 0, 0), 1)
    glass.sphere("dissolving", (1.2, 0, 0), 1)
    glass.write(out / "glass.obj")
    (out / "glass.mtl").write_text("""# PrismRT make_fixtures.py
newmtl refracting
Kd 0 0 0
Ks 1 1 1
Ns 1000
Ni 1.5
Tf 0.6 1 0.6
illum 7

newmtl dissolving
Kd 0.2 0.2 0.8
Ks 1 1 1
Ns 1000
Ni 1.5
d 0.3
illum 4
""", newline="\n")

    emissive = Obj("emissive.mtl")
    emissive.quad("floor", (0, 0, -0.5), 6)
    emissive.quad("light", (-1.2, 0, 0), 1.5)
    emissive.quad("glow", (1.2, 0, 0), 1.5)
    emissive.write(out / "emissive.obj")
    (out / "emissive.mtl").write_text("""# PrismRT make_fixtures.py
newmtl floor
Kd 0.5 0.5 0.5
Ks 0 0 0
illum 1

newmtl light
Kd 0 0 0
Ks 0 0 0
Ke 2 2 2
illum 1

newmtl glow
Kd 0 0 0
Ks 0 0 0
Ke 1 1 1
map_Ke glow.png
illum 1
""", newline="\n")

    pbr = Obj("pbr.mtl")
    pbr.sphere("gold", (-1.1, 1.1, 0), 0.9)
    pbr.sphere("velvet", (1.1, 1.1, 0), 0.9)
    pbr.sphere("lacquer", (-1.1, -1.1, 0), 0.9)
    pbr.sphere("brushed", (1.1, -1.1, 0), 0.9)
    pbr.write(out / "pbr.obj")
    (out / "pbr.mtl").write_text("""# PrismRT make_fixtures.py: the PBR extension (exocortex.com)
newmtl gold
Kd 1 0.766 0.336
Pr 0.25
Pm 1

newmtl velvet
Kd 0.6 0.05 0.1
Pr 0.8
Pm 0
Ps 1

newmtl lacquer
Kd 0.05 0.1 0.6
Pr 0.6
Pc 1
Pcr 0.05

newmtl brushed
Kd 0.9 0.9 0.9
Pm 1
Pr 1
map_Pr roughness.png
aniso 0.8
""", newline="\n")


if __name__ == "__main__":
    main()
