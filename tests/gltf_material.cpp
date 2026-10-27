// glTF -> MaterialX gltf_pbr conversion (src/materialx/gltf_material.cpp):
// the document structure for each mapped feature of the fixtures in
// tests/gltf, validity against the standard library, and MDL generation with
// the colour transforms into ACEScg.
//
// prism_gltf_material_test <tests/gltf directory>
// prism_gltf_material_test --check-normal-map <render of normal_map.gltf>
//   (camera at 0,0,6 looking at the origin, vertical FOV 40 degrees): each
//   normal-mapped half shades like the quad rotated to its mapped normal.
// prism_gltf_material_test --check-error-material <render>: the geometry of a
//   render whose materials failed shows the magenta error material.
#include "../src/materialx/gltf_material.h"
#include "../src/materialx/mdl_generator.h"
#include "../src/materialx/loader.h"
#include "../src/color/color_management.h"

#include <MaterialXCore/Material.h>
#include <MaterialXFormat/XmlIo.h>
#include <OpenImageIO/imagebuf.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace mx = MaterialX;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

tinygltf::Model Load(const std::filesystem::path& path)
{
    tinygltf::TinyGLTF loader;
    loader.SetImagesAsIs(true);
    tinygltf::Model model;
    std::string err, warn;
    const bool ok = path.extension() == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, path.string())
                                               : loader.LoadASCIIFromFile(&model, &err, &warn, path.string());
    if (!ok) throw std::runtime_error("cannot load " + path.string() + ": " + err);
    return model;
}

int MaterialIndex(const tinygltf::Model& model, const std::string& name)
{
    for (size_t i = 0; i < model.materials.size(); ++i)
        if (model.materials[i].name == name) return int(i);
    throw std::runtime_error("no material " + name);
}

struct Converted {
    prism::gltf::MaterialConversion conversion;
    std::set<int> images;
    mx::NodePtr shader;
};

Converted Convert(const tinygltf::Model& model, int material, bool vertexColor = false)
{
    Converted result;
    prism::gltf::MaterialConversionOptions options;
    options.vertexColor = vertexColor;
    options.imageFile = [&](int image) {
        result.images.insert(image);
        return "image_" + std::to_string(image) + ".png";
    };
    result.conversion = prism::gltf::ConvertGltfMaterial(model, material, options);
    auto materials = result.conversion.document->getMaterialNodes();
    if (materials.size() != 1 || materials[0]->getName() != prism::gltf::kMaterialName)
        throw std::runtime_error("expected one surfacematerial");
    auto shaders = mx::getShaderNodes(materials[0]);
    if (shaders.size() != 1 || shaders[0]->getCategory() != "gltf_pbr")
        throw std::runtime_error("expected one gltf_pbr shader");
    result.shader = shaders[0];
    return result;
}

mx::NodePtr Upstream(const mx::NodePtr& node, const std::string& input)
{
    auto port = node->getInput(input);
    return port ? port->getConnectedNode() : nullptr;
}

template <class T>
bool Value(const mx::NodePtr& node, const std::string& input, const T& expected)
{
    auto port = node ? node->getInput(input) : nullptr;
    if (!port || !port->getValue() || !port->getValue()->isA<T>()) return false;
    if constexpr (std::is_same_v<T, float>)
        return std::abs(port->getValue()->asA<float>() - expected) < 1e-5f;
    else
        return port->getValue()->asA<T>() == expected;
}

std::string ColorSpace(const mx::NodePtr& node, const std::string& input)
{
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getColorSpace() : "<missing>";
}

bool IsChannel(const mx::NodePtr& node, int index)
{
    return node && node->getCategory() == "extract" && Value(node, "index", index);
}

void TestFactors(const std::filesystem::path& dir)
{
    const auto model = Load(dir / "metal_rough_spheres.gltf");
    auto dielectric = Convert(model, 0);
    Check(Value(dielectric.shader, "base_color", mx::Color3(0.8f, 0.2f, 0.15f)), "factor base colour value");
    Check(ColorSpace(dielectric.shader, "base_color") == "lin_rec709", "base colour factor tagged lin_rec709");
    Check(Value(dielectric.shader, "metallic", 0.0f) && Value(dielectric.shader, "roughness", 0.1f),
          "metallic and roughness factors");
    Check(!dielectric.shader->getInput("alpha_mode") && !dielectric.shader->getInput("normal"),
          "opaque material without normal map sets neither");
    Check(dielectric.images.empty() && dielectric.conversion.warnings.empty(), "factor-only material");
    auto metal = Convert(model, 7);
    Check(Value(metal.shader, "metallic", 1.0f) && Value(metal.shader, "roughness", 1.0f), "metal factors");
}

void TestTextured(const std::filesystem::path& dir)
{
    const auto model = Load(dir / "textured.gltf");
    auto textured = Convert(model, MaterialIndex(model, "textured"));
    const auto& shader = textured.shader;
    // base_color <- convert <- multiply(factor) <- gltf_image color4 (sRGB)
    auto convert = Upstream(shader, "base_color");
    Check(convert && convert->getCategory() == "convert", "base colour converts color4 to color3");
    auto factor = convert ? Upstream(convert, "in") : nullptr;
    Check(factor && factor->getCategory() == "multiply" && ColorSpace(factor, "in2") == "lin_rec709" &&
              Value(factor, "in2", mx::Color4(1.0f, 0.9f, 0.8f, 1.0f)),
          "base colour factor multiplies the texture, tagged lin_rec709");
    auto base = factor ? Upstream(factor, "in1") : nullptr;
    Check(base && base->getCategory() == "gltf_image" && base->getType() == "color4", "base colour gltf_image");
    Check(ColorSpace(base, "file") == "srgb_texture", "base colour texture is sRGB");
    Check(Value(base, "offset", mx::Vector2(0.25f, 0.5f)) && Value(base, "scale", mx::Vector2(2.0f, 3.0f)) &&
              Value(base, "rotate", float(0.5 * 180.0 / 3.14159265358979323846)),
          "KHR_texture_transform offset/scale/rotation (degrees)");
    // Metallic (B) and roughness (G) from one raw lookup.
    auto metallic = Upstream(shader, "metallic"), roughness = Upstream(shader, "roughness");
    Check(IsChannel(metallic, 2) && IsChannel(roughness, 1), "metallic = B, roughness = G");
    auto orm = metallic ? Upstream(metallic, "in") : nullptr;
    Check(orm && orm == Upstream(roughness, "in") && orm->getType() == "vector3" &&
              ColorSpace(orm, "file").empty(),
          "metallic-roughness texture is one raw vector3 lookup");
    // Normal map with scale.
    auto normal = Upstream(shader, "normal");
    Check(normal && normal->getCategory() == "normalmap" && Value(normal, "scale", 0.5f) &&
              Upstream(normal, "in") && Upstream(normal, "in")->getCategory() == "gltf_image" &&
              ColorSpace(Upstream(normal, "in"), "file").empty(),
          "normal texture through normalmap with its scale, raw");
    // Emissive texture on TEXCOORD_1.
    auto emissive = Upstream(shader, "emissive");
    Check(emissive && emissive->getCategory() == "multiply" && ColorSpace(emissive, "in2") == "lin_rec709",
          "emissive factor multiplies the texture");
    auto emissiveImage = emissive ? Upstream(emissive, "in1") : nullptr;
    Check(emissiveImage && ColorSpace(emissiveImage, "file") == "srgb_texture", "emissive texture is sRGB");
    auto texcoord = emissiveImage ? Upstream(emissiveImage, "texcoord") : nullptr;
    Check(texcoord && texcoord->getCategory() == "geompropvalue" &&
              Value(texcoord, "geomprop", std::string(prism::gltf::kTexcoord1Primvar)),
          "TEXCOORD_1 is the TEXCOORD_1 primvar");
    Check(textured.conversion.usesTexcoord1 && !textured.conversion.usesVertexColor, "primvar use reported");
    Check(!Upstream(base, "texcoord"), "TEXCOORD_0 is the default texture coordinate");

    auto clamped = Convert(model, MaterialIndex(model, "clamped_vertex_color"), true);
    auto rgb = Upstream(clamped.shader, "base_color");
    auto colored = rgb ? Upstream(rgb, "in") : nullptr;
    Check(colored && colored->getCategory() == "multiply", "vertex colour multiplies the base colour");
    auto vertex = colored ? Upstream(colored, "in2") : nullptr;
    Check(vertex && vertex->getCategory() == "geompropvalue" && vertex->getType() == "color4" &&
              Value(vertex, "geomprop", std::string(prism::gltf::kColor0Primvar)),
          "COLOR_0 primvar");
    auto image = colored ? Upstream(colored, "in1") : nullptr;
    Check(image && Value(image, "uaddressmode", std::string("clamp")) &&
              Value(image, "vaddressmode", std::string("mirror")) &&
              Value(image, "filtertype", std::string("closest")),
          "sampler wrap modes and nearest filtering");
    Check(clamped.conversion.usesVertexColor, "vertex colour use reported");
    Check(!clamped.shader->getInput("alpha"), "opaque material ignores alpha");
}

void TestFeatures(const std::filesystem::path& dir)
{
    const auto model = Load(dir / "features.gltf");
    auto mask = Convert(model, MaterialIndex(model, "mask"));
    Check(Value(mask.shader, "alpha_mode", 1) && Value(mask.shader, "alpha_cutoff", 0.4f) &&
              IsChannel(Upstream(mask.shader, "alpha"), 3),
          "MASK: alpha mode, cutoff and texture alpha");
    auto blend = Convert(model, MaterialIndex(model, "blend"));
    Check(Value(blend.shader, "alpha_mode", 2) && Value(blend.shader, "alpha", 0.35f), "BLEND: factor alpha");
    auto glass = Convert(model, MaterialIndex(model, "glass"));
    Check(Value(glass.shader, "transmission", 1.0f) && Value(glass.shader, "ior", 1.45f) &&
              Value(glass.shader, "thickness", 1.0f) && Value(glass.shader, "attenuation_distance", 2.0f) &&
              Value(glass.shader, "attenuation_color", mx::Color3(0.9f, 0.6f, 0.3f)) &&
              ColorSpace(glass.shader, "attenuation_color").empty(),
          "transmission, volume and IOR (uniform attenuation colour converted, untagged)");
    auto clearcoat = Convert(model, MaterialIndex(model, "clearcoat"));
    Check(Value(clearcoat.shader, "clearcoat", 1.0f) && Value(clearcoat.shader, "clearcoat_roughness", 0.05f),
          "clearcoat");
    auto sheen = Convert(model, MaterialIndex(model, "sheen"));
    Check(Value(sheen.shader, "sheen_color", mx::Color3(1.0f, 0.8f, 0.9f)) &&
              ColorSpace(sheen.shader, "sheen_color") == "lin_rec709" && Value(sheen.shader, "sheen_roughness", 0.4f),
          "sheen");
    auto specular = Convert(model, MaterialIndex(model, "specular"));
    Check(Value(specular.shader, "specular", 0.8f) &&
              Value(specular.shader, "specular_color", mx::Color3(1.0f, 0.7f, 0.3f)),
          "specular");
    auto iridescence = Convert(model, MaterialIndex(model, "iridescence"));
    Check(Value(iridescence.shader, "iridescence", 1.0f) && Value(iridescence.shader, "iridescence_ior", 1.8f) &&
              Value(iridescence.shader, "iridescence_thickness", 550.0f),
          "iridescence (no thickness texture: the maximum)");
    auto emissive = Convert(model, MaterialIndex(model, "emissive"));
    Check(Value(emissive.shader, "emissive", mx::Color3(1.0f, 0.6f, 0.2f)) &&
              Value(emissive.shader, "emissive_strength", 4.0f),
          "emissive factor and strength");
    auto unlit = Convert(model, MaterialIndex(model, "unlit"));
    Check(Value(unlit.shader, "emissive", mx::Color3(0.2f, 0.7f, 0.9f)) &&
              Value(unlit.shader, "base_color", mx::Color3(0.0f)) && Value(unlit.shader, "specular", 0.0f) &&
              unlit.conversion.warnings.size() == 1,
          "unlit approximated as emission (with a warning)");
    auto anisotropy = Convert(model, MaterialIndex(model, "anisotropy"));
    auto strength = Upstream(anisotropy.shader, "anisotropy_strength");
    auto rotation = Upstream(anisotropy.shader, "anisotropy_rotation");
    Check(strength && strength->getCategory() == "multiply" && IsChannel(Upstream(strength, "in1"), 2) &&
              Value(strength, "in2", 0.8f),
          "anisotropy strength = factor * texture B");
    auto angle = rotation ? Upstream(rotation, "in1") : nullptr;
    Check(rotation && rotation->getCategory() == "add" && Value(rotation, "in2", 0.3f) && angle &&
              angle->getCategory() == "atan2",
          "anisotropy rotation = factor + atan2 of the texture direction");
    Check(anisotropy.conversion.warnings.empty(), "anisotropy texture supported");
    auto unknown = Convert(model, MaterialIndex(model, "unknown_extension"));
    Check(unknown.conversion.warnings.size() == 1 &&
              unknown.conversion.warnings[0].find("EXT_prism_unknown") != std::string::npos,
          "an unsupported material extension warns");
    const auto unsupported = prism::gltf::UnsupportedExtensions(model);
    Check(unsupported == std::vector<std::string>{"EXT_prism_unknown"}, "unsupported extensions listed");
}

void TestEmbedded(const std::filesystem::path& dir)
{
    const auto model = Load(dir / "embedded.glb");
    Check(model.images.size() == 1 && model.images[0].bufferView >= 0 && model.images[0].as_is &&
              !model.images[0].image.empty(),
          "GLB image kept encoded");
    auto textured = Convert(model, 0);
    Check(textured.images == std::set<int>{0}, "the buffer-view image is requested");
    auto fallback = Convert(model, -1);
    Check(Value(fallback.shader, "base_color", mx::Color3(1.0f)) && Value(fallback.shader, "metallic", 1.0f) &&
              Value(fallback.shader, "roughness", 1.0f) && fallback.images.empty(),
          "the default material is glTF's (white, metallic 1, roughness 1)");
}

// Every fixture material validates against the standard library and
// generates MDL; tagged colours convert into the working space.
void TestGeneration(const std::filesystem::path& dir, const std::string& libraries)
{
    const auto color = prism::color::ColorManagement::Create({});
    auto generator = MDLGenerator::shared(libraries);
    for (auto const* name : {"metal_rough_spheres.gltf", "textured.gltf", "features.gltf", "embedded.glb"}) {
        const auto model = Load(dir / name);
        for (int m = -1; m < int(model.materials.size()); ++m) {
            auto converted = Convert(model, m, true);
            auto doc = converted.conversion.document;
            doc->importLibrary(generator->get_stdlib());
            std::string message;
            Check(doc->validate(&message), std::string(name) + " material " + std::to_string(m) +
                                               " validates: " + message);
            auto generated = generator->generate(doc, prism::gltf::kMaterialName, *color);
            Check(generated.success, std::string(name) + " material " + std::to_string(m) +
                                         " generates MDL: " + generated.error_message);
            if (std::string(name) == "textured.gltf" && m == 0) {
                Check(generated.mdl_source.find("srgb_texture_to_acescg") != std::string::npos,
                      "sRGB textures convert into ACEScg");
                Check(generated.mdl_source.find("lin_rec709_to_acescg") != std::string::npos,
                      "lin_rec709 factors convert into ACEScg");
            }
        }
    }
}

// DocumentLoader's LayerGltfPbrVolume (src/materialx/loader.cpp): a gltf_pbr
// with a finite attenuation_distance gets a private copy of its graph with its
// transmission layered over its absorbing VDF; one without attenuation (the converter's 1e30) gets
// no volume; other lobes (clearcoat) and texture inputs survive.
void TestVolumeLayer(const std::filesystem::path& dir, const std::string& libraries)
{
    auto generator = MDLGenerator::shared(libraries);
    const auto path = std::filesystem::temp_directory_path() /
        ("prism_gltf_volume_layer_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".mtlx");
    {
        std::ofstream out(path);
        out << R"(<?xml version="1.0"?>
<materialx version="1.39">
  <image name="tint_image" type="color3"><input name="file" type="filename" value="tint.png" /></image>
  <gltf_pbr name="absorbing" type="surfaceshader">
    <input name="transmission" type="float" value="1" />
    <input name="base_color" type="color3" nodename="tint_image" />
    <input name="attenuation_distance" type="float" value="2" />
    <input name="attenuation_color" type="color3" value="0.5, 0.8, 0.9" />
    <input name="clearcoat" type="float" value="1" />
  </gltf_pbr>
  <surfacematerial name="Absorbing" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="absorbing" />
  </surfacematerial>
  <gltf_pbr name="clear" type="surfaceshader">
    <input name="transmission" type="float" value="1" />
    <input name="attenuation_distance" type="float" value="1e+30" />
  </gltf_pbr>
  <surfacematerial name="Clear" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="clear" />
  </surfacematerial>
</materialx>
)";
    }
    DocumentLoader loader(generator->get_stdlib());
    auto doc = loader.load(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    Check(bool(doc), "volume layer fixture loads");
    if (!doc) return;
    // Both nodes get a private copy of their graph (ClonePrivateGraph). The
    // clear glass's couples its reflection and transmission (an RT
    // dielectric) but has no volume layer. The absorbing one (textured
    // base_color) is coupled through prism_dielectric_split, its RT lobe over
    // the volume.
    auto graphOf = [&](const char* name) -> MaterialX::NodeGraphPtr {
        auto node = doc->getNode(name);
        auto implementation = node ? node->getImplementation("genmdl") : nullptr;
        auto graph = implementation ? implementation->asA<MaterialX::NodeGraph>() : nullptr;
        return graph && graph->getDocument() == doc ? graph : nullptr;
    };
    auto absorbingGraph = graphOf("absorbing"), clearGraph = graphOf("clear");
    Check(bool(absorbingGraph), "the attenuating gltf_pbr has a private graph");
    Check(bool(clearGraph) && clearGraph != absorbingGraph, "the clear glass is coupled in a graph of its own");
    int layers = 0, volumeLayers = 0;
    bool clearcoat = false;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto const& graph : {absorbingGraph, clearGraph})
        if (graph)
            for (auto const& node : graph->getNodes()) nodes.push_back(node);
    for (auto const& node : nodes) {
        if (node->getCategory() == "layer" && node->getType() == "BSDF") {
            auto top = Upstream(node, "top"), base = Upstream(node, "base");
            if (base && base->getCategory() == "anisotropic_vdf") ++volumeLayers;
            if (top && base && top->getCategory() == "dielectric_bsdf" &&
                top->getInput("scatter_mode") &&
                (top->getInput("scatter_mode")->getValueString() == "T" ||
                 top->getInput("scatter_mode")->getValueString() == "RT") &&
                base->getCategory() == "anisotropic_vdf") {
                ++layers;
                Check(!node->getDownstreamPorts().empty(), "the volume layer replaces the transmission downstream");
            }
        }
        if (node->getCategory() == "dielectric_bsdf" && !node->getInput("scatter_mode"))
            clearcoat = true;
    }
    Check(layers == 1 && volumeLayers == 1, "exactly one transmission is layered over its volume");
    Check(clearcoat, "the clearcoat lobe survives the expansion");
    bool tint = false;
    for (auto const& node : doc->getNodes())
        for (auto const& input : node->getInputs())
            if (input->getNodeName() == "tint_image") tint = true;
    Check(tint, "texture inputs stay connected");
    for (auto const* material : {"Absorbing", "Clear"}) {
        auto generated = generator->generate(doc, material, *prism::color::ColorManagement::Create({}));
        Check(generated.success, std::string("volume layer fixture ") + material + " generates MDL: " +
                                     generated.error_message);
    }
}

// Mean luminance of the image around a point of the z = 0 plane.
double Region(const OIIO::ImageBuf& image, double x, double y, double radius = 0.15)
{
    const auto& spec = image.spec();
    const double t = std::tan(20.0 * 3.14159265358979323846 / 180.0) * 6.0;
    const double aspect = double(spec.width) / spec.height;
    auto column = [&](double wx) { return int((wx / (t * aspect) * 0.5 + 0.5) * spec.width); };
    auto row = [&](double wy) { return int((0.5 - wy / t * 0.5) * spec.height); };
    double sum = 0;
    int count = 0;
    float pixel[4]{};
    for (int py = row(y + radius); py < row(y - radius); ++py)
        for (int px = column(x - radius); px < column(x + radius); ++px) {
            image.getpixel(px, py, pixel, 4);
            sum += 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
            ++count;
        }
    return count ? sum / count : 0.0;
}

// volume_sphere.gltf (camera at 0,0,6, a white background, linear Rec.709
// working space): through the sphere's centre light travels 2 units, one
// attenuationDistance, so Beer-Lambert leaves attenuationColor (0.5, 0.8,
// 0.9) of the background, within 5%; outside it is the untinted background.
int CheckVolume(const std::string& path)
{
    OIIO::ImageBuf image(path);
    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::cerr << "cannot read " << path << "\n";
        return 2;
    }
    const auto& spec = image.spec();
    const int cx = spec.width / 2, cy = spec.height / 2, r = std::max(1, spec.width / 32);
    double sum[3]{};
    int count = 0;
    float pixel[4]{};
    for (int y = cy - r; y < cy + r; ++y)
        for (int x = cx - r; x < cx + r; ++x) {
            image.getpixel(x, y, pixel, 4);
            for (int c = 0; c < 3; ++c) sum[c] += pixel[c];
            ++count;
        }
    const double expected[3] = {0.5, 0.8, 0.9};
    std::cout << "through the centre: " << sum[0] / count << ", " << sum[1] / count << ", " << sum[2] / count
              << " (expected 0.5, 0.8, 0.9)\n";
    for (int c = 0; c < 3; ++c)
        Check(std::abs(sum[c] / count - expected[c]) <= 0.05 * expected[c],
              "channel " + std::to_string(c) + " is tinted by Beer-Lambert absorption (5%)");
    image.getpixel(1, 1, pixel, 4);
    Check(std::abs(pixel[0] - 1.0f) < 0.02f && std::abs(pixel[2] - 1.0f) < 0.02f, "the background is white");
    return failures ? 1 : 0;
}

int CheckNormalMap(const std::string& path)
{
    OIIO::ImageBuf image(path);
    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::cerr << "cannot read " << path << "\n";
        return 2;
    }
    struct Pair { const char* name; double mapped, reference; };
    const Pair pairs[] = {
        {"U: left half (+x)", Region(image, -0.25, 0.8), Region(image, -1.6, 0.8)},
        {"U: right half (-x)", Region(image, 0.25, 0.8), Region(image, 1.6, 0.8)},
        {"V: top half (+y)", Region(image, 0.0, -0.55), Region(image, -1.6, -0.8)},
        {"V: bottom half (-y)", Region(image, 0.0, -1.05), Region(image, 1.6, -0.8)},
    };
    for (auto const& pair : pairs) {
        std::cout << pair.name << ": mapped " << pair.mapped << ", rotated quad " << pair.reference << "\n";
        Check(std::abs(pair.mapped - pair.reference) <= 0.2 * pair.reference,
              std::string(pair.name) + " shades like the rotated quad (20%)");
    }
    Check(pairs[0].mapped > 1.2 * pairs[1].mapped && pairs[2].mapped > 1.2 * pairs[3].mapped,
          "the halves of each normal map differ in the references' direction");
    return failures ? 1 : 0;
}

// A render whose materials all failed to load: the geometry shows the magenta
// error material (unlit emission, so exactly (1, 0, 1) in linear Rec.709)
// over a good part of the image.
int CheckErrorMaterial(const std::string& path)
{
    OIIO::ImageBuf image(path);
    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::cerr << "cannot read " << path << "\n";
        return 2;
    }
    const auto& spec = image.spec();
    int magenta = 0;
    float pixel[4]{};
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x) {
            image.getpixel(x, y, pixel, 4);
            if (pixel[0] > 0.5f && pixel[2] > 0.5f && pixel[1] < 0.05f * pixel[0]) ++magenta;
        }
    const double share = double(magenta) / (double(spec.width) * spec.height);
    std::cout << "magenta pixels: " << share * 100.0 << "%\n";
    Check(share > 0.05, "the failed materials render magenta");
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "--check-error-material") return CheckErrorMaterial(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--check-normal-map") return CheckNormalMap(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--check-volume") return CheckVolume(argv[2]);
        if (argc != 2) {
            std::cerr << "usage: prism_gltf_material_test <tests/gltf directory>\n";
            return 2;
        }
        const std::filesystem::path dir = argv[1];
        TestFactors(dir);
        TestTextured(dir);
        TestFeatures(dir);
        TestEmbedded(dir);
        if (const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH"); libraries && *libraries)
        {
            TestGeneration(dir, libraries);
            TestVolumeLayer(dir, libraries);
        }
        else
            std::cout << "PRISM_MATERIALX_LIBRARY_PATH not set: MDL generation not tested\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "glTF material conversion: all checks passed\n";
    return 0;
}
