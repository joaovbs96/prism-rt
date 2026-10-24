#include "obj_material.h"

#include <MaterialXCore/Value.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>

namespace mx = MaterialX;

namespace prism::obj {

namespace {

// MTL colours are linear Rec.709 (as Adobe's plugin and the former C++
// materials read them); the generator converts them into the working space
// once (docs/color_management.md). Colour maps are sRGB, data maps raw.
constexpr const char* kLinearSpace = "lin_rec709";
constexpr const char* kSrgbSpace = "srgb_texture";

std::string Lower(std::string text)
{
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string Format(double value)
{
    std::ostringstream text;
    text.precision(9);
    text << value;
    return text.str();
}

// Statements the mapping reads. Ka is read and ignored: ambient light has no
// meaning to a path tracer, and nearly every MTL file writes it.
bool Supported(const std::string& keyword)
{
    static const char* const supported[] = {
        "newmtl", "ka", "kd", "ks", "ke", "ns", "ni", "d", "tr", "illum", "tf",
        "map_kd", "map_ks", "map_ns", "map_d", "map_ke", "map_bump", "bump", "norm",
        "pr", "pm", "ps", "pc", "pcr", "aniso", "anisor",
        "map_pr", "map_pm", "map_ps", "map_pc", "map_pcr", "map_aniso", "disp"};
    return std::find(std::begin(supported), std::end(supported), keyword) != std::end(supported);
}

using Color = std::array<float, 3>;

Color ToColor(const tinyobj::real_t* v) { return {float(v[0]), float(v[1]), float(v[2])}; }
bool IsBlack(const Color& c) { return c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f; }

class Builder {
public:
    Builder(const tinyobj::material_t& material, const MaterialConversionOptions& options,
            MaterialConversion* result)
        : m_material(material), m_options(options), m_result(result)
    {
        m_doc = mx::createDocument();
        result->document = m_doc;
    }

    mx::DocumentPtr doc() const { return m_doc; }
    void warn(std::string message) { m_result->warnings.push_back(std::move(message)); }
    void unsupported(const std::string& what) { m_result->unsupported.insert(what); }

    // Whether the material authors a statement (tinyobjloader's defaults
    // count as absent when the statements are known).
    bool has(const char* keyword, bool differsFromDefault) const
    {
        if (m_options.statements) return m_options.statements->contains(keyword);
        return differsFromDefault;
    }
    bool explicitChannel(const char* keyword) const
    {
        return m_options.statements && m_options.statements->contains(std::string(keyword) + " -imfchan");
    }

    mx::NodePtr node(const std::string& category, const std::string& type, const std::string& stem)
    {
        return m_doc->addNode(category, m_doc->createValidChildName(stem), type);
    }

    static void connect(const mx::NodePtr& node, const std::string& input, const std::string& type,
                        const mx::NodePtr& source)
    {
        node->addInput(input, type)->setConnectedNode(source);
    }

    // The image lookup of a map, shared between inputs reading the same file
    // the same way. type: color3, color4 or vector3. shiftU, shiftV move the
    // lookup by whole texels (bump's finite differences). Null when the map
    // is not authored or its file is unavailable (warned).
    mx::NodePtr image(const std::string& file, const tinyobj::texture_option_t& option, const std::string& type,
                      bool color, const std::string& purpose, int shiftU = 0, int shiftV = 0)
    {
        if (file.empty()) return nullptr;
        const std::string name = m_options.imageFile ? m_options.imageFile(file) : std::string();
        if (name.empty()) {
            warn(purpose + ": cannot find '" + file + "'; ignored");
            return nullptr;
        }
        // Texture options. -o and -s place the map (u' = u * s + o); -clamp
        // clamps it; -colorspace (a tinyobjloader extension) names its
        // space. The others are ignored (one warning per load).
        if (option.brightness != 0.0f || option.contrast != 1.0f) unsupported("texture option -mm");
        if (option.turbulence[0] != 0.0f || option.turbulence[1] != 0.0f || option.turbulence[2] != 0.0f)
            unsupported("texture option -t");
        if (option.sharpness != 1.0f) unsupported("texture option -boost");
        if (!option.blendu || !option.blendv) unsupported("texture option -blendu/-blendv");
        if (option.texture_resolution != -1) unsupported("texture option -texres");
        if (option.origin_offset[2] != 0.0f || option.scale[2] != 1.0f)
            unsupported("texture option -o/-s third (w) component");
        std::string space;
        if (color) space = kSrgbSpace;
        const std::string requested = Lower(option.colorspace);
        if (requested == "srgb") space = kSrgbSpace;
        else if (requested == "linear") space = color ? kLinearSpace : "";
        else if (!requested.empty()) unsupported("texture option -colorspace " + option.colorspace);
        const bool placed = option.origin_offset[0] != 0.0f || option.origin_offset[1] != 0.0f ||
                            option.scale[0] != 1.0f || option.scale[1] != 1.0f;
        std::ostringstream key;
        key << type << '|' << space << '|' << name << '|' << option.clamp << '|' << Format(option.origin_offset[0])
            << ',' << Format(option.origin_offset[1]) << '|' << Format(option.scale[0]) << ','
            << Format(option.scale[1]) << '|' << shiftU << ',' << shiftV;
        if (auto found = m_images.find(key.str()); found != m_images.end()) return found->second;

        mx::NodePtr lookup = node("image", type, "image");
        mx::InputPtr fileInput = lookup->setInputValue("file", name, "filename");
        if (!space.empty()) fileInput->setColorSpace(space);
        if (option.clamp) {
            lookup->setInputValue("uaddressmode", std::string("clamp"));
            lookup->setInputValue("vaddressmode", std::string("clamp"));
        }
        mx::NodePtr uv;
        if (placed) {
            mx::NodePtr texcoord = node("texcoord", "vector2", "texcoord");
            mx::NodePtr scaled = node("multiply", "vector2", "texcoord_scale");
            connect(scaled, "in1", "vector2", texcoord);
            scaled->setInputValue("in2", mx::Vector2(float(option.scale[0]), float(option.scale[1])));
            uv = node("add", "vector2", "texcoord_offset");
            connect(uv, "in1", "vector2", scaled);
            uv->setInputValue("in2", mx::Vector2(float(option.origin_offset[0]), float(option.origin_offset[1])));
        }
        if (shiftU || shiftV) {
            std::array<int, 2> size{0, 0};
            if (m_options.imageSize) size = m_options.imageSize(file);
            if (size[0] <= 0 || size[1] <= 0) size = {1024, 1024};
            mx::NodePtr shifted = node("add", "vector2", "texcoord_texel");
            if (uv) connect(shifted, "in1", "vector2", uv);
            else connect(shifted, "in1", "vector2", node("texcoord", "vector2", "texcoord"));
            shifted->setInputValue("in2", mx::Vector2(float(shiftU) / float(size[0]), float(shiftV) / float(size[1])));
            uv = shifted;
        }
        if (uv) connect(lookup, "texcoord", "vector2", uv);
        m_images.emplace(key.str(), lookup);
        return lookup;
    }

    // bump: a height map as a tangent-space normal (MaterialX's
    // heighttonormal samples nothing around the point in generated MDL).
    // Central differences over one texel, in height units per texel, scaled
    // by -bm: n = normalize(-bm dh/du, -bm dh/dv, 1), encoded for normalmap.
    mx::NodePtr bumpNormal(const std::string& file, const tinyobj::texture_option_t& option)
    {
        if (file.empty()) return nullptr;
        int index = 0;
        if (explicitChannel("bump")) {
            switch (option.imfchan) {
            case 'g': index = 1; break;
            case 'b': index = 2; break;
            case 'm': case 'z': index = 3; break;
            default: index = 0; break;
            }
        }
        const std::string type = index == 3 ? "color4" : "vector3";
        auto height = [&](int du, int dv) -> mx::NodePtr {
            mx::NodePtr lookup = image(file, option, type, false, "bump", du, dv);
            return lookup ? channel(lookup, index) : nullptr;
        };
        mx::NodePtr right = height(1, 0), left = height(-1, 0), up = height(0, 1), down = height(0, -1);
        if (!right || !left || !up || !down) return nullptr;
        auto slope = [&](const mx::NodePtr& plus, const mx::NodePtr& minus, const std::string& stem) {
            mx::NodePtr difference = node("subtract", "float", stem);
            connect(difference, "in1", "float", plus);
            connect(difference, "in2", "float", minus);
            mx::NodePtr scaled = node("multiply", "float", stem + "_scaled");
            connect(scaled, "in1", "float", difference);
            scaled->setInputValue("in2", -0.5f * float(option.bump_multiplier));
            return scaled;
        };
        mx::NodePtr vector = node("combine3", "vector3", "bump_tangent_normal");
        connect(vector, "in1", "float", slope(right, left, "bump_du"));
        connect(vector, "in2", "float", slope(up, down, "bump_dv"));
        vector->setInputValue("in3", 1.0f);
        mx::NodePtr unit = node("normalize", "vector3", "bump_unit_normal");
        connect(unit, "in", "vector3", vector);
        mx::NodePtr encoded = node("multiply", "vector3", "bump_encoded");
        connect(encoded, "in1", "vector3", unit);
        encoded->setInputValue("in2", mx::Vector3(0.5f));
        mx::NodePtr biased = node("add", "vector3", "bump_encoded_biased");
        connect(biased, "in1", "vector3", encoded);
        biased->setInputValue("in2", mx::Vector3(0.5f));
        return biased;
    }

    mx::NodePtr channel(const mx::NodePtr& lookup, int index)
    {
        const std::string key = lookup->getName() + "|" + std::to_string(index);
        if (auto found = m_channels.find(key); found != m_channels.end()) return found->second;
        mx::NodePtr extract = node("extract", "float", lookup->getName() + "_" + "rgba"[index]);
        connect(extract, "in", lookup->getType(), lookup);
        extract->setInputValue("index", index);
        m_channels.emplace(key, extract);
        return extract;
    }

    // One channel of a scalar map: -imfchan r/g/b, m and z the alpha, l
    // (luminance) and the default the red channel (as Adobe's plugin reads
    // them: grey maps). map_d without -imfchan reads alpha when the image has
    // one.
    mx::NodePtr scalarMap(const char* keyword, const std::string& file, const tinyobj::texture_option_t& option,
                          const std::string& purpose)
    {
        if (file.empty()) return nullptr;
        int index = 0;
        if (explicitChannel(keyword)) {
            switch (option.imfchan) {
            case 'g': index = 1; break;
            case 'b': index = 2; break;
            case 'm': case 'z': index = 3; break;
            default: index = 0; break;
            }
        } else if (std::string(keyword) == "map_d" && m_options.hasAlpha && m_options.hasAlpha(file)) {
            index = 3;
        }
        mx::NodePtr lookup = image(file, option, index == 3 ? "color4" : "vector3", false, purpose);
        return lookup ? channel(lookup, index) : nullptr;
    }

    // factor * map (Adobe's rule: an authored factor other than the default
    // scales the map, except an explicit 0, which wins over the map; an
    // unauthored or default factor leaves the map alone). Null without a map.
    mx::NodePtr scaledScalar(const mx::NodePtr& map, std::optional<float> factor, float fallback,
                             const std::string& stem, float* value)
    {
        *value = factor.value_or(fallback);
        if (!map) return nullptr;
        if (!factor || *factor == fallback || *factor == 1.0f) return map;
        if (*factor == 0.0f) return nullptr;
        mx::NodePtr scaled = node("multiply", "float", stem);
        connect(scaled, "in1", "float", map);
        scaled->setInputValue("in2", *factor);
        return scaled;
    }

    // A float shader input from a value and a scalar map.
    void floatInput(const mx::NodePtr& shader, const std::string& input, std::optional<float> factor, float fallback,
                    const char* mapKeyword, const std::string& file, const tinyobj::texture_option_t& option)
    {
        float value = 0.0f;
        mx::NodePtr map = scaledScalar(scalarMap(mapKeyword, file, option, mapKeyword), factor, fallback, input, &value);
        if (map) connect(shader, input, "float", map);
        else if (factor || file.empty() == false) shader->setInputValue(input, value);
    }

    // A colour from a value (linear Rec.709) and a colour map (sRGB), or
    // null for "the value alone" (written by the caller).
    mx::NodePtr colorMap(std::optional<Color> factor, const std::string& file, const tinyobj::texture_option_t& option,
                         const std::string& stem, const std::string& purpose)
    {
        mx::NodePtr lookup = image(file, option, "color3", true, purpose);
        if (!lookup) return nullptr;
        if (factor && IsBlack(*factor)) return lookup; // Kd 0 0 0 beside a map is a placeholder (Maya)
        if (!factor || *factor == Color{1.0f, 1.0f, 1.0f}) return lookup;
        mx::NodePtr scaled = node("multiply", "color3", stem);
        connect(scaled, "in1", "color3", lookup);
        scaled->setInputValue("in2", mx::Color3((*factor)[0], (*factor)[1], (*factor)[2]))->setColorSpace(kLinearSpace);
        return scaled;
    }

    void colorInput(const mx::NodePtr& shader, const std::string& input, std::optional<Color> factor,
                    const std::string& file, const tinyobj::texture_option_t& option, const std::string& purpose)
    {
        if (mx::NodePtr map = colorMap(factor, file, option, input, purpose)) {
            connect(shader, input, "color3", map);
        } else if (factor) {
            shader->setInputValue(input, mx::Color3((*factor)[0], (*factor)[1], (*factor)[2]))
                ->setColorSpace(kLinearSpace);
        }
    }

    // An unknown_parameter map (map_Pc, map_Pcr, map_aniso: tinyobjloader
    // does not parse them): its file name, options other than none ignored.
    std::string extraMap(const char* keyword)
    {
        for (auto const& [key, value] : m_material.unknown_parameter) {
            if (Lower(key) != keyword) continue;
            std::istringstream tokens(value);
            std::string token, last;
            bool options = false;
            while (tokens >> token) {
                if (token.size() > 1 && token[0] == '-') options = true;
                last = token;
            }
            if (options) unsupported(std::string("texture options of ") + keyword);
            return last;
        }
        return {};
    }

private:
    const tinyobj::material_t& m_material;
    const MaterialConversionOptions& m_options;
    MaterialConversion* m_result;
    mx::DocumentPtr m_doc;
    std::map<std::string, mx::NodePtr> m_images, m_channels;
};

} // namespace

MtlStatements ScanMtlStatements(const std::string& text)
{
    MtlStatements result;
    std::set<std::string>* current = nullptr;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string keyword;
        if (!(tokens >> keyword) || keyword[0] == '#') continue;
        keyword = Lower(keyword);
        if (keyword == "newmtl") {
            std::string name;
            tokens >> name;
            current = &result[name];
            continue;
        }
        if (!current) continue;
        current->insert(keyword);
        std::string token;
        while (tokens >> token)
            if (Lower(token) == "-imfchan") current->insert(keyword + " -imfchan");
    }
    return result;
}

float PhongRoughness(float ns, const float ks[3])
{
    const float intensity = 0.2125f * ks[0] + 0.7154f * ks[1] + 0.0721f * ks[2];
    const float roughness = std::sqrt(2.0f / (std::max(0.0f, ns * intensity) + 2.0f));
    return roughness < 0.08f ? 0.0f : roughness;
}

MaterialConversion ConvertObjMaterial(const tinyobj::material_t& m, const MaterialConversionOptions& options)
{
    MaterialConversion result;
    Builder b(m, options, &result);
    mx::NodePtr shader = b.node("open_pbr_surface", "surfaceshader", "obj_shader");

    if (options.statements)
        for (auto const& statement : *options.statements)
            if (statement.find(' ') == std::string::npos && !Supported(statement))
                b.unsupported(statement == "map_ka" ? "map_Ka (ambient occlusion)" : statement);
    if (!options.statements)
        for (auto const& [key, value] : m.unknown_parameter)
            if (!Supported(Lower(key))) b.unsupported(Lower(key));
    // Reflection, displacement and decal maps tinyobjloader parses.
    if (!options.statements) {
        if (!m.ambient_texname.empty()) b.unsupported("map_Ka (ambient occlusion)");
        if (!m.reflection_texname.empty()) b.unsupported("refl");
    }

    auto value = [&](const char* keyword, float v, float tinyDefault) -> std::optional<float> {
        if (b.has(keyword, v != tinyDefault)) return v;
        return std::nullopt;
    };
    auto color = [&](const char* keyword, const tinyobj::real_t* v) -> std::optional<Color> {
        const Color c = ToColor(v);
        if (b.has(keyword, !IsBlack(c))) return c;
        return std::nullopt;
    };

    const int illum = b.has("illum", m.illum != 0) ? m.illum : 2;
    const bool glassDissolve = illum == 4 || illum == 9;   // transparency: glass on (from d)
    const bool glassRefraction = illum == 6 || illum == 7; // refraction on (through Tf)
    if (illum == 10) b.unsupported("illum 10 (shadow matte)");
    else if (illum < 0 || illum > 10) b.unsupported("illum " + std::to_string(illum));

    // Base colour: Kd x map_Kd. With map_Kd and no Kd, tinyobjloader's 0.6
    // is not authored; Kd absent altogether keeps OpenPBR's 0.8.
    std::optional<Color> kd = color("kd", m.diffuse);
    if (!options.statements && !m.diffuse_texname.empty() && kd && *kd == Color{0.6f, 0.6f, 0.6f}) kd.reset();
    b.colorInput(shader, "base_color", kd, m.diffuse_texname, m.diffuse_texopt, "map_Kd");

    const bool pbr = b.has("pr", m.roughness != 0.0f) || b.has("pm", m.metallic != 0.0f) ||
                     !m.roughness_texname.empty() || !m.metallic_texname.empty();
    if (pbr) {
        // The PBR extension: Pr, Pm (Ks and Ns are not read, as in Adobe's
        // plugin; without Pr, OpenPBR's roughness 0.3 stays).
        b.floatInput(shader, "specular_roughness", value("pr", m.roughness, 0.0f), 0.0f, "map_pr",
                     m.roughness_texname, m.roughness_texopt);
        b.floatInput(shader, "base_metalness", value("pm", m.metallic, 0.0f), 0.0f, "map_pm",
                     m.metallic_texname, m.metallic_texopt);
    } else {
        // Phong: roughness from Ks and Ns (Adobe's shininessToRoughness),
        // dielectric (metalness 0). Unauthored Ks and Ns are 0.5, as there.
        std::optional<Color> ks = color("ks", m.specular);
        std::optional<float> ns = value("ns", m.shininess, 1.0f);
        const bool ksMap = !m.specular_texname.empty(), nsMap = !m.specular_highlight_texname.empty();
        if (!ksMap && !nsMap) {
            const Color k = ks.value_or(Color{0.5f, 0.5f, 0.5f});
            const float kv[3] = {k[0], k[1], k[2]};
            shader->setInputValue("specular_roughness", PhongRoughness(ns.value_or(0.5f), kv));
        } else {
            // The same expression over the maps: luminance(Ks x map_Ks) and
            // Ns x map_Ns, per texel.
            mx::NodePtr intensity;
            mx::NodePtr ksNode = b.colorMap(ks, m.specular_texname, m.specular_texopt, "specular", "map_Ks");
            if (ksNode) {
                mx::NodePtr vector = b.node("convert", "vector3", "ks_vector");
                Builder::connect(vector, "in", "color3", ksNode);
                intensity = b.node("dotproduct", "float", "ks_intensity");
                Builder::connect(intensity, "in1", "vector3", vector);
                intensity->setInputValue("in2", mx::Vector3(0.2125f, 0.7154f, 0.0721f));
            }
            const Color k = ks.value_or(Color{0.5f, 0.5f, 0.5f});
            const float constantIntensity = 0.2125f * k[0] + 0.7154f * k[1] + 0.0721f * k[2];
            float nsValue = 0.0f;
            mx::NodePtr nsNode = b.scaledScalar(
                b.scalarMap("map_ns", m.specular_highlight_texname, m.specular_highlight_texopt, "map_Ns"), ns, 0.5f,
                "shininess", &nsValue);
            mx::NodePtr product = b.node("multiply", "float", "ns_intensity");
            if (nsNode) Builder::connect(product, "in1", "float", nsNode);
            else product->setInputValue("in1", nsValue);
            if (intensity) Builder::connect(product, "in2", "float", intensity);
            else product->setInputValue("in2", constantIntensity);
            mx::NodePtr clamped = b.node("max", "float", "ns_intensity_positive");
            Builder::connect(clamped, "in1", "float", product);
            clamped->setInputValue("in2", 0.0f);
            mx::NodePtr denominator = b.node("add", "float", "ns_plus_two");
            Builder::connect(denominator, "in1", "float", clamped);
            denominator->setInputValue("in2", 2.0f);
            mx::NodePtr quotient = b.node("divide", "float", "two_over");
            quotient->setInputValue("in1", 2.0f);
            Builder::connect(quotient, "in2", "float", denominator);
            mx::NodePtr root = b.node("sqrt", "float", "phong_roughness");
            Builder::connect(root, "in", "float", quotient);
            mx::NodePtr mirror = b.node("ifgreater", "float", "phong_roughness_mirror");
            Builder::connect(mirror, "value1", "float", root);
            mirror->setInputValue("value2", 0.08f);
            Builder::connect(mirror, "in1", "float", root);
            mirror->setInputValue("in2", 0.0f);
            Builder::connect(shader, "specular_roughness", "float", mirror);
        }
        // illum 0 and 1 have no highlights.
        if (illum == 0 || illum == 1) shader->setInputValue("specular_weight", 0.0f);
    }

    // PBR extension lobes.
    {
        std::optional<float> ps = value("ps", m.sheen, 0.0f);
        if (ps || !m.sheen_texname.empty())
            b.floatInput(shader, "fuzz_weight", ps, 0.0f, "map_ps", m.sheen_texname, m.sheen_texopt);
        const tinyobj::texture_option_t defaultOption = [] {
            tinyobj::texture_option_t o{};
            o.sharpness = 1; o.contrast = 1; o.scale[0] = o.scale[1] = o.scale[2] = 1;
            o.blendu = o.blendv = true; o.texture_resolution = -1; o.imfchan = 'm'; o.bump_multiplier = 1;
            return o;
        }();
        std::optional<float> pc = value("pc", m.clearcoat_thickness, 0.0f);
        const std::string pcMap = b.extraMap("map_pc");
        if (pc || !pcMap.empty()) b.floatInput(shader, "coat_weight", pc, 0.0f, "map_pc", pcMap, defaultOption);
        std::optional<float> pcr = value("pcr", m.clearcoat_roughness, 0.0f);
        const std::string pcrMap = b.extraMap("map_pcr");
        if (pcr || !pcrMap.empty())
            b.floatInput(shader, "coat_roughness", pcr, 0.0f, "map_pcr", pcrMap, defaultOption);
        std::optional<float> aniso = value("aniso", m.anisotropy, 0.0f);
        const std::string anisoMap = b.extraMap("map_aniso");
        if (aniso || !anisoMap.empty())
            b.floatInput(shader, "specular_roughness_anisotropy", aniso, 0.0f, "map_aniso", anisoMap, defaultOption);
        if (value("anisor", m.anisotropy_rotation, 0.0f).value_or(0.0f) != 0.0f)
            b.unsupported("anisor (OpenPBR has no anisotropy rotation)");
    }

    // Index of refraction. Ni 1 (no reflection at all, and tinyobjloader's
    // default) and below counts as unset: OpenPBR's 1.5 stays.
    if (std::optional<float> ni = value("ni", m.ior, 1.0f)) {
        if (*ni > 1.0001f) shader->setInputValue("specular_ior", *ni);
        else if (*ni < 0.9999f) b.warn("Ni " + Format(*ni) + " is below 1; using 1.5");
    }

    // Transmission (glass illum modes) and opacity (d, Tr, map_d).
    std::optional<float> d = b.has("d", false) || b.has("tr", false) || (!options.statements && m.dissolve != 1.0f)
        ? std::optional<float>(float(m.dissolve)) : std::nullopt;
    std::optional<Color> tf = color("tf", m.transmittance);
    if (tf && IsBlack(*tf)) tf.reset();
    if (glassDissolve || glassRefraction) {
        if (glassDissolve) {
            // The dissolve's transparency is the glass: 1 - d x map_d.
            float dv = 0.0f;
            mx::NodePtr map = b.scaledScalar(b.scalarMap("map_d", m.alpha_texname, m.alpha_texopt, "map_d"), d, 1.0f,
                                             "dissolve", &dv);
            if (map) {
                mx::NodePtr inverted = b.node("subtract", "float", "transmission");
                inverted->setInputValue("in1", 1.0f);
                Builder::connect(inverted, "in2", "float", map);
                Builder::connect(shader, "transmission_weight", "float", inverted);
            } else {
                shader->setInputValue("transmission_weight", 1.0f - std::clamp(dv, 0.0f, 1.0f));
            }
        } else {
            shader->setInputValue("transmission_weight", 1.0f);
            if (d && *d < 1.0f) shader->setInputValue("geometry_opacity", std::clamp(*d, 0.0f, 1.0f));
        }
        if (tf) shader->setInputValue("transmission_color", mx::Color3((*tf)[0], (*tf)[1], (*tf)[2]))
                    ->setColorSpace(kLinearSpace);
    } else {
        if (tf && *tf != Color{1.0f, 1.0f, 1.0f}) b.unsupported("Tf without a refraction illum (4, 6, 7, 9)");
        float dv = 1.0f;
        mx::NodePtr map = b.scaledScalar(b.scalarMap("map_d", m.alpha_texname, m.alpha_texopt, "map_d"), d, 1.0f,
                                         "opacity", &dv);
        if (map) Builder::connect(shader, "geometry_opacity", "float", map);
        else if (d && dv < 1.0f) shader->setInputValue("geometry_opacity", std::clamp(dv, 0.0f, 1.0f));
    }

    // Emission: Ke x map_Ke as radiance (emission_luminance 1).
    {
        std::optional<Color> ke = color("ke", m.emission);
        mx::NodePtr map = b.colorMap(ke, m.emissive_texname, m.emissive_texopt, "emission", "map_Ke");
        if (map || (ke && !IsBlack(*ke))) {
            if (map) Builder::connect(shader, "emission_color", "color3", map);
            else shader->setInputValue("emission_color", mx::Color3((*ke)[0], (*ke)[1], (*ke)[2]))
                     ->setColorSpace(kLinearSpace);
            shader->setInputValue("emission_luminance", 1.0f);
        }
    }

    // Normals: norm (a tangent-space normal map) or bump (a height map), -bm
    // scaling either.
    {
        mx::NodePtr normal;
        float scale = 1.0f;
        if (mx::NodePtr lookup = b.image(m.normal_texname, m.normal_texopt, "vector3", false, "norm")) {
            normal = lookup;
            scale = float(m.normal_texopt.bump_multiplier);
            if (!m.bump_texname.empty()) b.warn("both norm and bump are given; bump is ignored");
        } else {
            normal = b.bumpNormal(m.bump_texname, m.bump_texopt);
        }
        if (normal) {
            mx::NodePtr map = b.node("normalmap", "vector3", "normal_map");
            Builder::connect(map, "in", "vector3", normal);
            if (scale != 1.0f) map->setInputValue("scale", scale);
            Builder::connect(shader, "geometry_normal", "vector3", map);
        }
    }

    // Displacement: disp as a scalar height along the normal (-mm base gain:
    // height = base + gain x texel), bound to the material's
    // displacementshader input as USD MaterialX displacement reaches the
    // Hydra path (AddDisplacementShader).
    mx::NodePtr displacement;
    if (!m.displacement_texname.empty()) {
        tinyobj::texture_option_t option = m.displacement_texopt;
        const float base = float(option.brightness), gain = float(option.contrast);
        option.brightness = 0.0f; // -mm is honoured here, not ignored
        option.contrast = 1.0f;
        if (mx::NodePtr height = b.scalarMap("disp", m.displacement_texname, option, "disp")) {
            displacement = b.node("displacement", "displacementshader", "obj_displacement");
            if (base != 0.0f) {
                mx::NodePtr scaled = b.node("multiply", "float", "disp_gain");
                Builder::connect(scaled, "in1", "float", height);
                scaled->setInputValue("in2", gain);
                mx::NodePtr offset = b.node("add", "float", "disp_base");
                Builder::connect(offset, "in1", "float", scaled);
                offset->setInputValue("in2", base);
                Builder::connect(displacement, "displacement", "float", offset);
            } else {
                Builder::connect(displacement, "displacement", "float", height);
                if (gain != 1.0f) displacement->setInputValue("scale", gain);
            }
            result.displacement = true;
        }
    }
    mx::NodePtr surfaceMaterial = b.doc()->addMaterialNode(kMaterialName, shader);
    if (displacement)
        surfaceMaterial->addInput("displacementshader", "displacementshader")->setConnectedNode(displacement);
    for (auto& warning : result.warnings) warning = "material '" + m.name + "': " + warning;
    return result;
}

} // namespace prism::obj
