// OBJ/MTL -> MaterialX open_pbr_surface conversion
// (src/materialx/obj_material.cpp): the document structure for each mapped
// statement of the fixtures in tests/obj and of inline MTL text, validity
// against the standard library, and MDL generation.
//
// prism_obj_material_test <tests/obj directory>
// prism_obj_material_test --check-emissive <render of emissive.obj>
// prism_obj_material_test --check-textured <render of textured.obj>
// prism_obj_material_test --check-glass <render of glass.obj>
//   (camera at 0,0,6 looking at the origin, vertical FOV 45 degrees).
#include "../src/materialx/obj_material.h"
#include "../src/materialx/mdl_generator.h"
#include "../src/materialx/loader.h"
#include "../src/color/color_management.h"

#include <MaterialXCore/Material.h>
#include <OpenImageIO/imagebuf.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

struct Library {
    std::vector<tinyobj::material_t> materials;
    prism::obj::MtlStatements statements;
};

Library Parse(const std::string& text)
{
    Library library;
    std::map<std::string, int> names;
    std::string warn, err;
    std::istringstream stream(text);
    tinyobj::LoadMtl(&names, &library.materials, &stream, &warn, &err);
    library.statements = prism::obj::ScanMtlStatements(text);
    return library;
}

Library Load(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return Parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

struct Converted {
    prism::obj::MaterialConversion conversion;
    mx::NodePtr shader;
};

Converted Convert(const Library& library, const std::string& name, bool withAlpha = true)
{
    for (auto const& material : library.materials) {
        if (material.name != name) continue;
        Converted result;
        prism::obj::MaterialConversionOptions options;
        options.imageFile = [](const std::string& map) { return "published_" + map; };
        options.hasAlpha = [withAlpha](const std::string& map) { return withAlpha && map == "leaf.png"; };
        auto statements = library.statements.find(name);
        if (statements != library.statements.end()) options.statements = &statements->second;
        result.conversion = prism::obj::ConvertObjMaterial(material, options);
        auto materials = result.conversion.document->getMaterialNodes();
        if (materials.size() != 1 || materials[0]->getName() != prism::obj::kMaterialName)
            throw std::runtime_error("expected one surfacematerial");
        auto shaders = mx::getShaderNodes(materials[0]);
        if (shaders.size() != 1 || shaders[0]->getCategory() != "open_pbr_surface")
            throw std::runtime_error("expected one open_pbr_surface shader");
        result.shader = shaders[0];
        return result;
    }
    throw std::runtime_error("no material " + name);
}

mx::NodePtr Upstream(const mx::NodePtr& node, const std::string& input)
{
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getConnectedNode() : nullptr;
}

template <class T>
bool Value(const mx::NodePtr& node, const std::string& input, const T& expected)
{
    auto port = node ? node->getInput(input) : nullptr;
    if (!port || !port->getValue() || !port->getValue()->isA<T>()) return false;
    if constexpr (std::is_same_v<T, float>)
        return std::abs(port->getValue()->asA<float>() - expected) < 1e-4f;
    else
        return port->getValue()->asA<T>() == expected;
}

std::string ColorSpace(const mx::NodePtr& node, const std::string& input)
{
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getColorSpace() : "<missing>";
}

bool IsImage(const mx::NodePtr& node, const std::string& type, const std::string& file)
{
    return node && node->getCategory() == "image" && node->getType() == type &&
        Value(node, "file", std::string("published_" + file));
}

void TestStatements()
{
    auto statements = prism::obj::ScanMtlStatements(
        "# comment\nnewmtl a\n  Kd 1 1 1\nmap_Pr -imfchan g rough.png\nnewmtl b\nPR 0.5\n\nTr 0.2\n");
    Check(statements["a"].contains("kd") && statements["a"].contains("map_pr") &&
          statements["a"].contains("map_pr -imfchan"), "statements of a, with an explicit channel");
    Check(statements["b"].contains("pr") && statements["b"].contains("tr") && !statements["b"].contains("kd"),
          "statements of b, lower case");
    const float half[3] = {0.5f, 0.5f, 0.5f}, white[3] = {1.0f, 1.0f, 1.0f};
    Check(std::abs(prism::obj::PhongRoughness(200.0f, half) - std::sqrt(2.0f / 102.0f)) < 1e-6f,
          "Phong roughness sqrt(2 / (Ns * luminance(Ks) + 2))");
    Check(prism::obj::PhongRoughness(1000.0f, white) == 0.0f, "Phong roughness below 0.08 is a mirror");
    Check(std::abs(prism::obj::PhongRoughness(1.0f, half) - std::sqrt(2.0f / 2.5f)) < 1e-6f, "Ns 1 is rough");
}

void TestTextured(const std::filesystem::path& dir)
{
    const auto library = Load(dir / "textured.mtl");
    auto checker = Convert(library, "checker");
    auto base = Upstream(checker.shader, "base_color");
    Check(IsImage(base, "color3", "checker.png"), "base_color is map_Kd (Kd 1 1 1 does not scale it)");
    Check(ColorSpace(base, "file") == "srgb_texture", "map_Kd is sRGB");
    auto offset = Upstream(base, "texcoord");
    auto scale = Upstream(offset, "in1");
    Check(offset && offset->getCategory() == "add" && scale && scale->getCategory() == "multiply" &&
          Value(scale, "in2", mx::Vector2(2.0f, 2.0f)), "map_Kd -s 2 2 scales the texture coordinates");
    auto mirror = Upstream(checker.shader, "specular_roughness");
    Check(mirror && mirror->getCategory() == "ifgreater", "map_Ks/map_Ns: roughness is computed per texel");
    Check(checker.conversion.unsupported.empty() && checker.conversion.warnings.empty(), "checker maps cleanly");

    auto cutout = Convert(library, "cutout");
    auto opacity = Upstream(cutout.shader, "geometry_opacity");
    Check(opacity && opacity->getCategory() == "extract" && Value(opacity, "index", 3) &&
          IsImage(Upstream(opacity, "in"), "color4", "leaf.png"), "map_d reads the alpha of an RGBA map");
    Check(ColorSpace(Upstream(opacity, "in"), "file").empty(), "map_d is raw");
    auto cutoutRed = Convert(library, "cutout", false);
    Check(Value(Upstream(cutoutRed.shader, "geometry_opacity"), "index", 0), "map_d reads red without alpha");

    auto bumped = Convert(library, "bumped");
    auto normal = Upstream(bumped.shader, "geometry_normal");
    Check(normal && normal->getCategory() == "normalmap" && Upstream(normal, "in") &&
          Upstream(normal, "in")->getCategory() == "add", "bump: an encoded tangent-space normal into normalmap");
    int heights = 0, slopes = 0;
    for (auto const& node : bumped.conversion.document->getNodes()) {
        if (IsImage(node, "vector3", "height.png") && Upstream(node, "texcoord")) ++heights;
        if (node->getCategory() == "multiply" && Value(node, "in2", -1.0f)) ++slopes;
    }
    Check(heights == 4 && slopes == 2, "bump -bm 2: central differences of four shifted lookups, scaled by -bm / 2");
    Check(Value(bumped.shader, "base_color", mx::Color3(0.8f)) &&
          ColorSpace(bumped.shader, "base_color") == "lin_rec709", "Kd is linear Rec.709");
    Check(Value(bumped.shader, "specular_roughness", prism::obj::PhongRoughness(20.0f, std::array<float, 3>{0.1f, 0.1f, 0.1f}.data())),
          "Phong roughness from Ks and Ns");

    auto normalMapped = Convert(library, "normal");
    auto map = Upstream(normalMapped.shader, "geometry_normal");
    Check(map && map->getCategory() == "normalmap" && IsImage(Upstream(map, "in"), "vector3", "normal.png"),
          "norm: a raw vector3 normal map");
}

void TestGlass(const std::filesystem::path& dir)
{
    const auto library = Load(dir / "glass.mtl");
    auto refracting = Convert(library, "refracting");
    Check(Value(refracting.shader, "transmission_weight", 1.0f), "illum 7 transmits");
    Check(Value(refracting.shader, "transmission_color", mx::Color3(0.6f, 1.0f, 0.6f)) &&
          ColorSpace(refracting.shader, "transmission_color") == "lin_rec709", "Tf tints the transmission");
    Check(Value(refracting.shader, "specular_ior", 1.5f) && Value(refracting.shader, "specular_roughness", 0.0f),
          "Ni and a mirror-smooth Ns");
    Check(!refracting.shader->getInput("geometry_opacity"), "glass is not cut out");
    auto dissolving = Convert(library, "dissolving");
    Check(Value(dissolving.shader, "transmission_weight", 0.7f), "illum 4: the dissolve is glass (1 - d)");
    Check(!dissolving.shader->getInput("geometry_opacity"), "illum 4 does not cut out");
}

void TestEmissive(const std::filesystem::path& dir)
{
    const auto library = Load(dir / "emissive.mtl");
    auto light = Convert(library, "light");
    Check(Value(light.shader, "emission_color", mx::Color3(2.0f)) && Value(light.shader, "emission_luminance", 1.0f) &&
          ColorSpace(light.shader, "emission_color") == "lin_rec709", "Ke is radiance (luminance 1)");
    Check(Value(light.shader, "specular_weight", 0.0f), "illum 1 has no highlights");
    auto glow = Convert(library, "glow");
    Check(IsImage(Upstream(glow.shader, "emission_color"), "color3", "glow.png") &&
          Value(glow.shader, "emission_luminance", 1.0f), "map_Ke (Ke 1 1 1 does not scale it)");
    auto floor = Convert(library, "floor");
    Check(!floor.shader->getInput("emission_luminance"), "no Ke: no emission");
}

void TestPbr(const std::filesystem::path& dir)
{
    const auto library = Load(dir / "pbr.mtl");
    auto gold = Convert(library, "gold");
    Check(Value(gold.shader, "base_metalness", 1.0f) && Value(gold.shader, "specular_roughness", 0.25f),
          "Pm and Pr");
    Check(!gold.shader->getInput("specular_weight"), "the PBR extension keeps the specular lobe");
    auto velvet = Convert(library, "velvet");
    Check(Value(velvet.shader, "fuzz_weight", 1.0f) && Value(velvet.shader, "base_metalness", 0.0f), "Ps is fuzz");
    auto lacquer = Convert(library, "lacquer");
    Check(Value(lacquer.shader, "coat_weight", 1.0f) && Value(lacquer.shader, "coat_roughness", 0.05f),
          "Pc and Pcr are the coat");
    auto brushed = Convert(library, "brushed");
    auto roughness = Upstream(brushed.shader, "specular_roughness");
    Check(roughness && roughness->getCategory() == "extract" && Value(roughness, "index", 0) &&
          IsImage(Upstream(roughness, "in"), "vector3", "roughness.png"),
          "map_Pr (Pr 1 does not scale it), red channel");
    Check(Value(brushed.shader, "specular_roughness_anisotropy", 0.8f), "aniso");
}

void TestDefaultsAndUnsupported()
{
    const auto library = Parse(R"(newmtl mapped
map_Kd albedo.png
Ns 10
newmtl odd
Kd 0.5 0.5 0.5
Tf 0.5 0.5 0.5
illum 10
refl -type sphere sky.png
map_Ka -mm 0 2 ao.png
sharpness 60
newmtl zero
Kd 0 0 0
map_Kd albedo.png
Pr 0
map_Pr -imfchan g rough.png
Ni 1.0
newmtl missing
map_Kd missing.png
)");
    auto mapped = Convert(library, "mapped");
    Check(IsImage(Upstream(mapped.shader, "base_color"), "color3", "albedo.png"),
          "map_Kd without Kd is the map alone (not tinyobjloader's 0.6 times it)");
    Check(Value(mapped.shader, "specular_roughness", prism::obj::PhongRoughness(10.0f, std::array<float, 3>{0.5f, 0.5f, 0.5f}.data())),
          "unauthored Ks is 0.5 (Adobe)");
    auto odd = Convert(library, "odd");
    auto const& unsupported = odd.conversion.unsupported;
    Check(unsupported.contains("refl") && unsupported.contains("sharpness") && unsupported.contains("illum 10 (shadow matte)") &&
          unsupported.contains("Tf without a refraction illum (4, 6, 7, 9)") &&
          unsupported.contains("map_Ka (ambient occlusion)"), "unsupported statements are reported");
    auto zero = Convert(library, "zero");
    Check(IsImage(Upstream(zero.shader, "base_color"), "color3", "albedo.png"), "Kd 0 0 0 beside map_Kd is a placeholder");
    Check(Value(Upstream(zero.shader, "specular_roughness"), "index", 1),
          "Pr 0 (the default) beside map_Pr is a placeholder; -imfchan g reads green");
    Check(!zero.shader->getInput("specular_ior"), "Ni 1 counts as unset");
    prism::obj::MaterialConversionOptions options;
    options.imageFile = [](const std::string&) { return std::string(); };
    auto missing = prism::obj::ConvertObjMaterial(library.materials[3], options);
    Check(missing.warnings.size() == 1 && missing.warnings[0].find("cannot find 'missing.png'") != std::string::npos,
          "a missing map warns");
}

const char* const kDisplacementMtl = R"(newmtl displaced
Kd 0.5 0.5 0.5
disp -mm 0.1 0.5 -imfchan g height.png
newmtl plain
Kd 0.5 0.5 0.5
disp -s 2 2 1 height.png
)";

// disp: a displacementshader bound to the surfacematerial (the shape USD
// MaterialX displacement has), -mm base gain as base + gain x texel.
void TestDisplacement()
{
    const auto library = Parse(kDisplacementMtl);
    auto displaced = Convert(library, "displaced");
    auto materials = displaced.conversion.document->getMaterialNodes();
    auto shader = Upstream(materials.front(), "displacementshader");
    Check(shader && shader->getCategory() == "displacement" && shader->getType() == "displacementshader",
          "disp: a displacement shader on the material's displacementshader input");
    auto base = Upstream(shader, "displacement");
    auto gain = Upstream(base, "in1");
    auto channel = Upstream(gain, "in1");
    Check(base && base->getCategory() == "add" && Value(base, "in2", 0.1f) && gain &&
          gain->getCategory() == "multiply" && Value(gain, "in2", 0.5f), "-mm 0.1 0.5: height = 0.1 + 0.5 x texel");
    Check(channel && Value(channel, "index", 1) && IsImage(Upstream(channel, "in"), "vector3", "height.png") &&
          ColorSpace(Upstream(channel, "in"), "file").empty(), "-imfchan g of a raw map");
    Check(displaced.conversion.displacement && displaced.conversion.unsupported.empty(),
          "disp is converted, -mm is not reported as ignored");
    auto plain = Convert(library, "plain");
    auto plainShader = Upstream(plain.conversion.document->getMaterialNodes().front(), "displacementshader");
    auto red = Upstream(plainShader, "displacement");
    Check(red && Value(red, "index", 0) && !plainShader->getInput("scale") &&
          Upstream(Upstream(red, "in"), "texcoord"), "disp reads red by default, placed by -s");
}

void TestGeneration(const std::filesystem::path& dir, const std::string& libraries)
{
    prism::color::SetActive(prism::color::ColorManagement::Create({}));
    {
        auto generator = MDLGenerator::shared(libraries);
        const auto library = Parse(kDisplacementMtl);
        for (auto const* name : {"displaced", "plain"}) {
            auto doc = Convert(library, name).conversion.document;
            doc->importLibrary(generator->get_stdlib());
            std::string message;
            Check(doc->validate(&message), std::string("displacement material ") + name + " validates: " + message);
            auto generated = generator->generate(doc, prism::obj::kMaterialName);
            Check(generated.success, std::string("displacement material ") + name + " generates MDL: " +
                                         generated.error_message);
        }
    }
    prism::color::SetActive(prism::color::ColorManagement::Create({}));
    auto generator = MDLGenerator::shared(libraries);
    for (auto const* name : {"textured.mtl", "glass.mtl", "emissive.mtl", "pbr.mtl"}) {
        const auto library = Load(dir / name);
        for (auto const& material : library.materials) {
            auto converted = Convert(library, material.name);
            auto doc = converted.conversion.document;
            doc->importLibrary(generator->get_stdlib());
            std::string message;
            const std::string label = std::string(name) + " " + material.name;
            Check(doc->validate(&message), label + " validates: " + message);
            auto generated = generator->generate(doc, prism::obj::kMaterialName);
            Check(generated.success, label + " generates MDL: " + generated.error_message);
            if (material.name == "checker")
                Check(generated.mdl_source.find("srgb_texture_to_acescg") != std::string::npos,
                      "sRGB maps convert into ACEScg");
        }
    }
}

bool ReadImage(const std::string& path, OIIO::ImageBuf* image)
{
    *image = OIIO::ImageBuf(path);
    if (!image->read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::cerr << "cannot read " << path << "\n";
        return false;
    }
    return true;
}

// The mean of a square around a point given in world units on the z = 0
// plane (camera at 0,0,6, vertical FOV 45 degrees).
std::array<double, 3> Mean(const OIIO::ImageBuf& image, double x, double y, double half = 0.15)
{
    const auto& spec = image.spec();
    const double extent = 6.0 * std::tan(22.5 * 3.14159265358979323846 / 180.0);
    auto px = [&](double wx) { return int((wx / extent * 0.5 + 0.5) * spec.width); };
    auto py = [&](double wy) { return int((0.5 - wy / extent * 0.5) * spec.height); };
    std::array<double, 3> sum{};
    int count = 0;
    float pixel[4]{};
    for (int j = py(y + half); j <= py(y - half); ++j)
        for (int i = px(x - half); i <= px(x + half); ++i) {
            image.getpixel(i, j, pixel, 4);
            for (int c = 0; c < 3; ++c) sum[c] += pixel[c];
            ++count;
        }
    for (auto& s : sum) s /= std::max(count, 1);
    return sum;
}

// emissive.obj (black environment, linear Rec.709 working space): the Ke 2
// emitter shows radiance 2, the map_Ke one its texture's colours.
int CheckEmissive(const std::string& path)
{
    OIIO::ImageBuf image;
    if (!ReadImage(path, &image)) return 2;
    const auto light = Mean(image, -1.2, 0.0, 0.3);
    std::cout << "Ke 2 emitter: " << light[0] << ", " << light[1] << ", " << light[2] << "\n";
    for (double c : light) Check(std::abs(c - 2.0) < 0.02, "the Ke 2 2 2 emitter has radiance 2");
    const auto glow = Mean(image, 1.2, 0.0, 0.6);
    std::cout << "map_Ke emitter mean: " << glow[0] << ", " << glow[1] << ", " << glow[2] << "\n";
    Check(glow[0] > 0.2 && glow[0] > glow[2] * 2.0, "the map_Ke emitter shows its warm texture");
    return failures ? 1 : 0;
}

// textured.obj (studio environment): the cutout quad's transparent corner
// shows the environment (as the background just beside the quad), its opaque
// centre the leaf; the bump and normal maps vary the shading across their
// quads.
int CheckTextured(const std::string& path)
{
    OIIO::ImageBuf image;
    if (!ReadImage(path, &image)) return 2;
    const auto corner = Mean(image, 1.1 + 0.9, 1.1 + 0.9, 0.04);
    const auto beside = Mean(image, 1.1 + 1.2, 1.1 + 0.9, 0.04);
    const auto centre = Mean(image, 1.1, 1.1, 0.2);
    std::cout << "cutout corner: " << corner[0] << ", " << corner[1] << ", " << corner[2] << "; beside the quad: "
              << beside[0] << ", " << beside[1] << ", " << beside[2] << "; centre: " << centre[0] << ", "
              << centre[1] << ", " << centre[2] << "\n";
    for (int c = 0; c < 3; ++c)
        Check(std::abs(corner[c] - beside[c]) <= 0.1 * beside[c] + 0.01, "map_d cuts out the corner");
    Check(centre[1] > centre[0] * 1.5, "the opaque centre is the green leaf");
    // Shading detail along a horizontal line: the RMS of its departure from
    // a straight line (the environment's smooth gradient), relative to its
    // mean. The leaf's flat centre is the reference for noise.
    auto detail = [&](double x, double y) {
        std::vector<double> values;
        // The bump and normal maps vary along x only: a band of rows averages
        // the noise away.
        for (double dx = -0.6; dx <= 0.6 + 1e-9; dx += 0.05) {
            double sum = 0.0;
            for (double dy = -0.3; dy <= 0.3 + 1e-9; dy += 0.05) sum += Mean(image, x + dx, y + dy, 0.02)[1];
            values.push_back(sum / 13.0);
        }
        const double n = double(values.size());
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            sx += double(i); sy += values[i]; sxx += double(i) * i; sxy += double(i) * values[i];
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), intercept = (sy - slope * sx) / n;
        double residual = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            const double d = values[i] - (intercept + slope * double(i));
            residual += d * d;
        }
        return std::sqrt(residual / n) / std::max(sy / n, 1e-6);
    };
    const double flat = detail(1.1, 1.1), bump = detail(-1.1, -1.1), normal = detail(1.1, -1.1);
    std::cout << "shading detail: flat " << flat << ", bump " << bump << ", normal map " << normal << "\n";
    Check(bump > 3.0 * flat, "bump varies the shading");
    Check(normal > 3.0 * flat, "norm varies the shading");
    return failures ? 1 : 0;
}

// glass.obj (white environment): through the refracting sphere's centre the
// white background is tinted by Tf (0.6, 1, 0.6), twice.
int CheckGlass(const std::string& path)
{
    OIIO::ImageBuf image;
    if (!ReadImage(path, &image)) return 2;
    const auto centre = Mean(image, -1.2, 0.0, 0.1);
    std::cout << "through the refracting sphere: " << centre[0] << ", " << centre[1] << ", " << centre[2] << "\n";
    Check(centre[1] > 0.6 && centre[0] < centre[1] * 0.6 && centre[2] < centre[1] * 0.6,
          "the transmission is tinted green");
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "--check-emissive") return CheckEmissive(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--check-textured") return CheckTextured(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--check-glass") return CheckGlass(argv[2]);
        if (argc != 2) {
            std::cerr << "usage: prism_obj_material_test <tests/obj directory>\n";
            return 2;
        }
        const std::filesystem::path dir = argv[1];
        TestStatements();
        TestTextured(dir);
        TestGlass(dir);
        TestEmissive(dir);
        TestPbr(dir);
        TestDefaultsAndUnsupported();
        TestDisplacement();
        if (const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH"); libraries && *libraries)
            TestGeneration(dir, libraries);
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
    std::cout << "OBJ material conversion: all checks passed\n";
    return 0;
}
