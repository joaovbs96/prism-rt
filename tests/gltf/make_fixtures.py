"""Writes the synthetic glTF fixtures of tests/gltf (docs/gltf.md).

python tests/gltf/make_fixtures.py [output directory, default: this one]

Small, deterministic files: spheres and quads with factor-only and textured
gltf_pbr materials, the KHR_materials_* extensions the converter maps,
KHR_texture_transform, TEXCOORD_1, COLOR_0, samplers, external, data-URI and
GLB buffer-view images, normal maps beside geometric references, and
studio_env.hdr to light them.
"""
import base64
import json
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


def checker(a, b, cells=4, size=16):
    return png(size, size, lambda x, y: a if ((x * cells // size) + (y * cells // size)) % 2 == 0 else b)


class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0", "generator": "PrismRT make_fixtures.py"}, "scene": 0,
                    "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [], "accessors": [],
                    "bufferViews": [], "buffers": []}
        self.blob = bytearray()
        self.extensions = set()

    def view(self, data, target=None, stride=None):
        while len(self.blob) % 4:
            self.blob += b"\x00"
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        if stride:
            view["byteStride"] = stride
        self.blob += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind, components, component_type=5126, normalized=False, target=34962):
        fmt = {5126: "f", 5121: "B", 5123: "H", 5125: "I"}[component_type]
        flat = [v for value in values for v in (value if isinstance(value, (list, tuple)) else [value])]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        accessor = {"bufferView": self.view(data, target), "componentType": component_type,
                    "count": len(values), "type": kind}
        if normalized:
            accessor["normalized"] = True
        if kind == "VEC3" and component_type == 5126:
            accessor["min"] = [min(v[i] for v in values) for i in range(3)]
            accessor["max"] = [max(v[i] for v in values) for i in range(3)]
        self.doc["accessors"].append(accessor)
        return len(self.doc["accessors"]) - 1

    def mesh(self, positions, normals, uvs, indices, material, uv1=None, colors=None):
        # Meshes of the same geometry share its accessors.
        key = id(positions), id(uvs)
        cache = self.__dict__.setdefault("shared", {})
        if key not in cache:
            index_type = 5123 if len(positions) < 65536 else 5125
            cache[key] = ({"POSITION": self.accessor(positions, "VEC3", 3),
                           "NORMAL": self.accessor(normals, "VEC3", 3),
                           "TEXCOORD_0": self.accessor(uvs, "VEC2", 2)},
                          self.accessor(indices, "SCALAR", 1, index_type, target=34963))
        attributes, index_accessor = dict(cache[key][0]), cache[key][1]
        if uv1:
            attributes["TEXCOORD_1"] = self.accessor(uv1, "VEC2", 2)
        if colors:
            # Normalized unsigned bytes: KHR_mesh_quantization-style data.
            attributes["COLOR_0"] = self.accessor(colors, "VEC4", 4, 5121, True)
        primitive = {"attributes": attributes, "indices": index_accessor, "material": material}
        self.doc["meshes"].append({"primitives": [primitive]})
        return len(self.doc["meshes"]) - 1

    def node(self, mesh, translation=(0, 0, 0), scale=None):
        node = {"mesh": mesh, "translation": list(translation)}
        if scale:
            node["scale"] = list(scale)
        self.doc["nodes"].append(node)
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]) - 1)

    def material(self, material):
        for name in material.get("extensions", {}):
            self.extensions.add(name)
        self.doc["materials"].append(material)
        return len(self.doc["materials"]) - 1

    def image(self, data=None, uri=None, embed="uri", sampler=None):
        """A texture of a PNG: an external file (uri), a data URI or a GLB buffer view."""
        if uri:
            image = {"uri": uri}
        elif embed == "data":
            image = {"uri": "data:image/png;base64," + base64.b64encode(data).decode()}
        else:
            image = {"bufferView": self.view(data), "mimeType": "image/png"}
        self.doc.setdefault("images", []).append(image)
        texture = {"source": len(self.doc["images"]) - 1}
        if sampler is not None:
            texture["sampler"] = sampler
        self.doc.setdefault("textures", []).append(texture)
        return len(self.doc["textures"]) - 1

    def sampler(self, **values):
        self.doc.setdefault("samplers", []).append(values)
        return len(self.doc["samplers"]) - 1

    def finish(self):
        if self.extensions:
            self.doc["extensionsUsed"] = sorted(self.extensions)
        for key in ("materials", "accessors", "bufferViews"):
            if not self.doc[key]:
                del self.doc[key]

    def write_gltf(self, path):
        self.finish()
        self.doc["buffers"] = [{"byteLength": len(self.blob),
                                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(self.blob)).decode()}]
        path.write_text(json.dumps(self.doc, indent=1) + "\n", newline="\n")

    def write_glb(self, path):
        self.finish()
        while len(self.blob) % 4:
            self.blob += b"\x00"
        self.doc["buffers"] = [{"byteLength": len(self.blob)}]
        text = json.dumps(self.doc, separators=(",", ":")).encode()
        text += b" " * ((4 - len(text) % 4) % 4)
        body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(self.blob), 0x004E4942) + self.blob
        path.write_bytes(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def sphere(segments=24, rings=12):
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2 * math.pi * s / segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            positions.append(n)
            normals.append(n)
            uvs.append((s / segments, r / rings))
    for r in range(rings):
        for s in range(segments):
            a, b = r * (segments + 1) + s, (r + 1) * (segments + 1) + s
            indices += [a, b, a + 1, a + 1, b, b + 1]
    return positions, normals, uvs, indices


def quad(size=1.0):
    """A square in the XY plane facing +Z; glTF UV (0, 0) at the top left."""
    h = size / 2
    positions = [(-h, -h, 0), (h, -h, 0), (h, h, 0), (-h, h, 0)]
    return positions, [(0, 0, 1)] * 4, [(0, 1), (1, 1), (1, 0), (0, 0)], [0, 1, 2, 0, 2, 3]


def metal_rough_spheres(out):
    g = Gltf()
    mesh_data = sphere()
    colors = [(0.8, 0.2, 0.15, 1), (0.2, 0.6, 0.2, 1), (0.2, 0.3, 0.8, 1), (0.9, 0.9, 0.9, 1)]
    roughness = [0.1, 0.35, 0.65, 1.0]
    for row, metallic in enumerate((0.0, 1.0)):
        for column in range(4):
            material = g.material({"name": "m%d_r%d" % (metallic, column), "pbrMetallicRoughness": {
                "baseColorFactor": list(colors[column]), "metallicFactor": metallic,
                "roughnessFactor": roughness[column]}})
            mesh = g.mesh(*mesh_data, material)
            g.node(mesh, (2.4 * column - 3.6, 1.2 - 2.4 * row, 0))
    g.write_gltf(out / "metal_rough_spheres.gltf")


def textured(out):
    g = Gltf()
    (out / "textured_base.png").write_bytes(checker((230, 200, 60, 255), (40, 70, 160, 255)))
    # ORM: R occlusion, G roughness, B metallic.
    (out / "textured_orm.png").write_bytes(
        png(16, 16, lambda x, y: (255, 40 + 13 * y, 255 if x >= 8 else 0, 255)))
    # A bumpy tangent-space normal map (+Y up).
    def bump(x, y):
        dx = math.sin(2 * math.pi * x / 8) * 0.5
        n = (dx, 0.0, 1.0)
        length = math.sqrt(sum(c * c for c in n))
        return tuple(int(round((c / length * 0.5 + 0.5) * 255)) for c in n) + (255,)
    (out / "textured_normal.png").write_bytes(png(16, 16, bump))
    clamp = g.sampler(wrapS=33071, wrapT=33648, magFilter=9728)
    base = g.image(uri="textured_base.png")
    base_clamped = g.image(uri="textured_base.png", sampler=clamp)
    orm = g.image(uri="textured_orm.png")
    normal = g.image(uri="textured_normal.png")
    emissive = g.image(checker((255, 255, 255, 255), (0, 0, 0, 255), 2), embed="data")
    transform = {"KHR_texture_transform": {"offset": [0.25, 0.5], "rotation": 0.5, "scale": [2, 3]}}
    g.extensions.add("KHR_texture_transform")
    m0 = g.material({"name": "textured", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": base, "extensions": transform},
        "baseColorFactor": [1, 0.9, 0.8, 1],
        "metallicRoughnessTexture": {"index": orm}, "metallicFactor": 1, "roughnessFactor": 1},
        "normalTexture": {"index": normal, "scale": 0.5},
        "occlusionTexture": {"index": orm},
        "emissiveTexture": {"index": emissive, "texCoord": 1}, "emissiveFactor": [0.5, 0.25, 0.1]})
    m1 = g.material({"name": "clamped_vertex_color", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": base_clamped}, "metallicFactor": 0, "roughnessFactor": 0.5}})
    p, n, uv, idx = quad(2.0)
    uv1 = [(u * 2, v * 2) for u, v in uv]
    g.node(g.mesh(p, n, uv, idx, m0, uv1=uv1), (-1.1, 0, 0))
    colors = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 128)]
    uv_wide = [(u * 1.5 - 0.25, v * 1.5 - 0.25) for u, v in uv]
    g.node(g.mesh(p, n, uv_wide, idx, m1, colors=colors), (1.1, 0, 0))
    g.write_gltf(out / "textured.gltf")


def features(out):
    g = Gltf()
    s = sphere(20, 10)
    p, n, uv, idx = quad(1.8)
    leaf = g.image(png(16, 16, lambda x, y: (60, 160, 60, 255 if (x - 8) ** 2 + (y - 8) ** 2 < 40 else 0)), embed="data")
    materials = [
        ("mask", {"pbrMetallicRoughness": {"baseColorTexture": {"index": leaf}, "metallicFactor": 0},
                  "alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": True}, "quad"),
        ("blend", {"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.3, 0.3, 0.35], "metallicFactor": 0},
                   "alphaMode": "BLEND", "doubleSided": True}, "quad"),
        ("glass", {"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0},
                   "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1},
                                  "KHR_materials_volume": {"thicknessFactor": 1, "attenuationDistance": 2,
                                                           "attenuationColor": [0.9, 0.6, 0.3]},
                                  "KHR_materials_ior": {"ior": 1.45}}}, "sphere"),
        ("clearcoat", {"pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.05, 0.05, 1], "metallicFactor": 0,
                                                "roughnessFactor": 0.6},
                       "extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1, "clearcoatRoughnessFactor": 0.05}}},
         "sphere"),
        ("sheen", {"pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.1, 0.4, 1], "metallicFactor": 0},
                   "extensions": {"KHR_materials_sheen": {"sheenColorFactor": [1, 0.8, 0.9], "sheenRoughnessFactor": 0.4}}},
         "sphere"),
        ("specular", {"pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.2, 0.2, 1], "metallicFactor": 0,
                                               "roughnessFactor": 0.2},
                      "extensions": {"KHR_materials_specular": {"specularFactor": 0.8,
                                                                "specularColorFactor": [1, 0.7, 0.3]}}}, "sphere"),
        ("iridescence", {"pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1], "metallicFactor": 1,
                                                  "roughnessFactor": 0.15},
                         "extensions": {"KHR_materials_iridescence": {"iridescenceFactor": 1, "iridescenceIor": 1.8,
                                                                      "iridescenceThicknessMaximum": 550}}}, "sphere"),
        ("emissive", {"pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1], "metallicFactor": 0},
                      "emissiveFactor": [1, 0.6, 0.2],
                      "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}}}, "sphere"),
        ("unlit", {"pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.7, 0.9, 1]},
                   "extensions": {"KHR_materials_unlit": {}}}, "sphere"),
        ("anisotropy", {"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.9, 0.9, 1], "metallicFactor": 1,
                                                 "roughnessFactor": 0.4},
                        "extensions": {"KHR_materials_anisotropy": {
                            "anisotropyStrength": 0.8, "anisotropyRotation": 0.3,
                            "anisotropyTexture": {"index": g.image(png(4, 4, lambda x, y: (255, 128, 255, 255)),
                                                                   embed="data")}}}}, "sphere"),
        ("unknown_extension", {"pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7, 0.7, 1], "metallicFactor": 0},
                               "extensions": {"EXT_prism_unknown": {"value": 1}}}, "sphere"),
    ]
    for i, (name, material, shape) in enumerate(materials):
        material["name"] = name
        index = g.material(material)
        mesh = g.mesh(*(s if shape == "sphere" else (p, n, uv, idx)), index)
        g.node(mesh, (2.2 * (i % 6) - 5.5, 1.1 - 2.2 * (i // 6), 0), (0.9, 0.9, 0.9))
    g.write_gltf(out / "features.gltf")


def embedded(out):
    g = Gltf()
    texture = g.image(checker((200, 40, 40, 255), (240, 240, 240, 255), 8, 32), embed="view")
    material = g.material({"name": "embedded", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": texture}, "metallicFactor": 0, "roughnessFactor": 0.5}})
    g.node(g.mesh(*sphere(), material))
    # A primitive without a material: the glTF default material.
    p, n, uv, idx = quad(3.0)
    mesh = g.mesh(p, n, uv, idx, None)
    del g.doc["meshes"][mesh]["primitives"][0]["material"]
    g.node(mesh, (0, 0, -1.2))
    g.write_glb(out / "embedded.glb")


def normal_map(out):
    """Normal maps against geometry: flat quads whose normal maps tilt each
    half by asin(0.6), beside quads rotated by the same angle (references).
    Row 0 tilts along U (left half toward +x), row 1 along V (top half toward
    +y; +Y of a glTF normal map is up in the image). Checks the V flip and
    the tangent frame (prism_gltf_material_test --check-normal-map)."""
    g = Gltf()
    s, c = 0.6, 0.8

    def encode(n):
        return tuple(int(round((v * 0.5 + 0.5) * 255)) for v in n) + (255,)

    def material(texture=None):
        material = {"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1], "metallicFactor": 0,
                                             "roughnessFactor": 0.9}}
        if texture is not None:
            material["normalTexture"] = {"index": texture}
        return g.material(material)

    plain = material()
    along_u = material(g.image(png(16, 16, lambda x, y: encode((s if x < 8 else -s, 0, c))), embed="data"))
    along_v = material(g.image(png(16, 16, lambda x, y: encode((0, s if y < 8 else -s, c))), embed="data"))
    p, n, uv, idx = quad(1.0)
    half = math.asin(s) / 2
    reference = g.mesh(p, n, uv, idx, plain)

    def place(mesh, translation, rotation=None):
        node = {"mesh": mesh, "translation": list(translation)}
        if rotation:
            node["rotation"] = rotation
        g.doc["nodes"].append(node)
        g.doc["scenes"][0]["nodes"].append(len(g.doc["nodes"]) - 1)

    place(reference, (-1.6, 0.8, 0), [0, math.sin(half), 0, math.cos(half)])
    place(g.mesh(p, n, uv, idx, along_u), (0, 0.8, 0))
    place(reference, (1.6, 0.8, 0), [0, -math.sin(half), 0, math.cos(half)])
    place(reference, (-1.6, -0.8, 0), [-math.sin(half), 0, 0, math.cos(half)])
    place(g.mesh(p, n, uv, idx, along_v), (0, -0.8, 0))
    place(reference, (1.6, -0.8, 0), [math.sin(half), 0, 0, math.cos(half)])
    g.write_gltf(out / "normal_map.gltf")


def volume_sphere(out):
    """A closed glass sphere (IOR 1: no refraction or reflection) absorbing
    through KHR_materials_volume. Through its centre (path length 2) the
    background is tinted by attenuationColor^(2 / attenuationDistance), here
    attenuationColor itself (prism_gltf_material_test --check-volume)."""
    g = Gltf()
    material = g.material({"name": "absorbing", "pbrMetallicRoughness": {
        "baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0},
        "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1},
                       "KHR_materials_ior": {"ior": 1.0},
                       "KHR_materials_volume": {"thicknessFactor": 2, "attenuationDistance": 2,
                                                "attenuationColor": [0.5, 0.8, 0.9]}}})
    g.node(g.mesh(*sphere(48, 24), material))
    g.write_gltf(out / "volume_sphere.gltf")


def environment(out, width=64, height=32):
    """A small studio-like equirect environment in Radiance HDR."""
    def rgbe(r, g, b):
        v = max(r, g, b)
        if v < 1e-32:
            return bytes((0, 0, 0, 0))
        m, e = math.frexp(v)
        scale = m * 256.0 / v
        return bytes((int(r * scale), int(g * scale), int(b * scale), e + 128))

    rows = bytearray()
    for y in range(height):
        theta = math.pi * (y + 0.5) / height
        for x in range(width):
            # phi is the angle from +X toward +Z: the direction is
            # (sin theta cos phi, cos theta, sin theta sin phi), placed in the
            # image by UsdLux's lat-long convention (the image's centre is +Z,
            # its first quarter +X), so column x looks toward phi = 2 pi (x +
            # 0.5) / width - pi / 2.
            phi = (2 * math.pi * (x + 0.5) / width - math.pi / 2) % (2 * math.pi)
            sky = max(math.cos(theta), 0.0)
            r, g, b = 0.25 + 0.5 * sky, 0.3 + 0.55 * sky, 0.35 + 0.7 * sky
            if math.cos(theta) < 0:
                r, g, b = 0.12, 0.1, 0.08
            # A warm key light and a cool rim.
            if abs(phi - 1.2) < 0.35 and abs(theta - 0.9) < 0.3:
                r, g, b = 24.0, 20.0, 16.0
            if abs(phi - 4.2) < 0.2 and abs(theta - 1.2) < 0.2:
                r, g, b = 4.0, 6.0, 9.0
            rows += rgbe(r, g, b)
    header = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width)
    (out / "studio_env.hdr").write_bytes(header + bytes(rows))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent
    out.mkdir(parents=True, exist_ok=True)
    metal_rough_spheres(out)
    textured(out)
    features(out)
    embedded(out)
    normal_map(out)
    volume_sphere(out)
    environment(out)


if __name__ == "__main__":
    main()
