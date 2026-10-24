#include "gltf_material.h"

#include <MaterialXCore/Value.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace mx = MaterialX;

namespace prism::gltf {

namespace {

// Linear Rec.709, which glTF's factors are authored in; the generator
// converts them into the working space once (docs/color_management.md).
constexpr const char* kLinearSpace = "lin_rec709";
constexpr const char* kSrgbSpace = "srgb_texture";

double Number(const tinygltf::Value& object, const char* key, double fallback)
{
    if (!object.IsObject() || !object.Has(key)) return fallback;
    const tinygltf::Value& value = object.Get(key);
    return value.IsNumber() || value.IsInt() ? value.GetNumberAsDouble() : fallback;
}

std::vector<double> Numbers(const tinygltf::Value& object, const char* key, std::vector<double> fallback)
{
    if (!object.IsObject() || !object.Has(key)) return fallback;
    const tinygltf::Value& value = object.Get(key);
    if (!value.IsArray() || value.ArrayLen() != fallback.size()) return fallback;
    for (size_t i = 0; i < fallback.size(); ++i)
        if (value.Get(int(i)).IsNumber() || value.Get(int(i)).IsInt())
            fallback[i] = value.Get(int(i)).GetNumberAsDouble();
    return fallback;
}

// A texture reference: core textureInfo or an extension's texture object,
// with KHR_texture_transform applied.
struct TextureRef {
    int texture = -1;
    int texCoord = 0;
    std::array<double, 2> offset{0, 0}, scale{1, 1};
    double rotation = 0; // radians
    double normalScale = 1;
    bool valid() const { return texture >= 0; }
};

void ApplyTransform(const tinygltf::ExtensionMap& extensions, TextureRef* ref)
{
    auto found = extensions.find("KHR_texture_transform");
    if (found == extensions.end()) return;
    const tinygltf::Value& t = found->second;
    const auto offset = Numbers(t, "offset", {0, 0});
    const auto scale = Numbers(t, "scale", {1, 1});
    ref->offset = {offset[0], offset[1]};
    ref->scale = {scale[0], scale[1]};
    ref->rotation = Number(t, "rotation", 0);
    if (t.Has("texCoord") && t.Get("texCoord").IsInt()) ref->texCoord = t.Get("texCoord").GetNumberAsInt();
}

TextureRef Ref(const tinygltf::TextureInfo& info)
{
    TextureRef ref;
    ref.texture = info.index;
    ref.texCoord = info.texCoord;
    ApplyTransform(info.extensions, &ref);
    return ref;
}

TextureRef Ref(const tinygltf::NormalTextureInfo& info)
{
    TextureRef ref;
    ref.texture = info.index;
    ref.texCoord = info.texCoord;
    ref.normalScale = info.scale;
    ApplyTransform(info.extensions, &ref);
    return ref;
}

// An extension's texture object ("clearcoatTexture": {"index": 0, ...}).
TextureRef Ref(const tinygltf::Value& extension, const char* key)
{
    TextureRef ref;
    if (!extension.IsObject() || !extension.Has(key)) return ref;
    const tinygltf::Value& info = extension.Get(key);
    if (!info.IsObject() || !info.Has("index") || !info.Get("index").IsInt()) return ref;
    ref.texture = info.Get("index").GetNumberAsInt();
    if (info.Has("texCoord") && info.Get("texCoord").IsInt()) ref.texCoord = info.Get("texCoord").GetNumberAsInt();
    ref.normalScale = Number(info, "scale", 1);
    if (info.Has("extensions") && info.Get("extensions").IsObject()) {
        tinygltf::ExtensionMap extensions;
        for (auto const& key : info.Get("extensions").Keys())
            extensions[key] = info.Get("extensions").Get(key);
        ApplyTransform(extensions, &ref);
    }
    return ref;
}

std::string Format(double value)
{
    std::ostringstream text;
    text.precision(9);
    text << value;
    return text.str();
}

class Builder {
public:
    Builder(const tinygltf::Model& model, const MaterialConversionOptions& options, MaterialConversion* result)
        : m_model(model), m_options(options), m_result(result)
    {
        m_doc = mx::createDocument();
        result->document = m_doc;
    }

    mx::DocumentPtr doc() const { return m_doc; }
    void warn(std::string message) { m_result->warnings.push_back(std::move(message)); }

    mx::NodePtr node(const std::string& category, const std::string& type, const std::string& stem)
    {
        return m_doc->addNode(category, m_doc->createValidChildName(stem), type);
    }

    static void connect(const mx::NodePtr& node, const std::string& input, const std::string& type,
                        const mx::NodePtr& source)
    {
        node->addInput(input, type)->setConnectedNode(source);
    }

    // The gltf_image lookup of a texture, shared between the inputs reading
    // the same texture the same way (metallic and roughness, for instance).
    // type: color3, color4, vector3 or float. Null when the texture or its
    // image is unusable (warned).
    mx::NodePtr image(const TextureRef& ref, const std::string& type, bool srgb, const std::string& purpose)
    {
        if (!ref.valid()) return nullptr;
        if (ref.texture >= int(m_model.textures.size())) {
            warn(purpose + ": texture " + std::to_string(ref.texture) + " does not exist; ignored");
            return nullptr;
        }
        const tinygltf::Texture& texture = m_model.textures[size_t(ref.texture)];
        int source = texture.source;
        if (source < 0) {
            // KHR_texture_basisu / EXT_texture_webp sources are not read.
            for (auto const& [name, ext] : texture.extensions)
                if (name == "EXT_texture_webp" && ext.Has("source") && ext.Get("source").IsInt())
                    source = ext.Get("source").GetNumberAsInt();
        }
        if (source < 0 || source >= int(m_model.images.size())) {
            warn(purpose + ": texture " + std::to_string(ref.texture) + " has no supported image; ignored");
            return nullptr;
        }
        const std::string file = m_options.imageFile ? m_options.imageFile(source) : std::string();
        if (file.empty()) {
            warn(purpose + ": image " + std::to_string(source) + " is unavailable; ignored");
            return nullptr;
        }
        int texCoord = ref.texCoord;
        if (texCoord > 1) {
            warn(purpose + ": TEXCOORD_" + std::to_string(texCoord) + " is not supported; using TEXCOORD_0");
            texCoord = 0;
        }
        std::string uMode = "periodic", vMode = "periodic", filter = "linear";
        if (texture.sampler >= 0 && texture.sampler < int(m_model.samplers.size())) {
            const tinygltf::Sampler& sampler = m_model.samplers[size_t(texture.sampler)];
            auto wrap = [](int mode) {
                switch (mode) {
                case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return "clamp";
                case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return "mirror";
                default: return "periodic";
                }
            };
            uMode = wrap(sampler.wrapS);
            vMode = wrap(sampler.wrapT);
            if (sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST) filter = "closest";
        }
        std::ostringstream key;
        key << type << '|' << srgb << '|' << file << '|' << texCoord << '|' << Format(ref.offset[0]) << ','
            << Format(ref.offset[1]) << '|' << Format(ref.scale[0]) << ',' << Format(ref.scale[1]) << '|'
            << Format(ref.rotation) << '|' << uMode << vMode << filter;
        if (auto found = m_images.find(key.str()); found != m_images.end()) return found->second;

        mx::NodePtr lookup = node("gltf_image", type, "image");
        mx::InputPtr fileInput = lookup->setInputValue("file", file, "filename");
        if (srgb) fileInput->setColorSpace(kSrgbSpace);
        if (texCoord == 1) connect(lookup, "texcoord", "vector2", texcoord1());
        if (ref.offset != std::array<double, 2>{0, 0})
            lookup->setInputValue("offset", mx::Vector2(float(ref.offset[0]), float(ref.offset[1])));
        if (ref.scale != std::array<double, 2>{1, 1})
            lookup->setInputValue("scale", mx::Vector2(float(ref.scale[0]), float(ref.scale[1])));
        if (ref.rotation != 0)
            lookup->setInputValue("rotate", float(ref.rotation * 180.0 / 3.14159265358979323846));
        if (uMode != "periodic") lookup->setInputValue("uaddressmode", uMode);
        if (vMode != "periodic") lookup->setInputValue("vaddressmode", vMode);
        if (filter != "linear") lookup->setInputValue("filtertype", filter);
        m_images.emplace(key.str(), lookup);
        return lookup;
    }

    // One channel of a lookup (vector3 or color4).
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

    // factor * channel of a texture: a float shader input.
    void floatInput(const mx::NodePtr& shader, const std::string& input, double factor, const TextureRef& ref,
                    int channelIndex, const std::string& purpose)
    {
        const std::string type = channelIndex == 3 ? "color4" : "vector3";
        if (mx::NodePtr lookup = image(ref, type, false, purpose)) {
            mx::NodePtr value = channel(lookup, channelIndex);
            if (factor != 1.0) {
                mx::NodePtr scaled = node("multiply", "float", input);
                connect(scaled, "in1", "float", value);
                scaled->setInputValue("in2", float(factor));
                value = scaled;
            }
            connect(shader, input, "float", value);
        } else {
            shader->setInputValue(input, float(factor));
        }
    }

    // factor (linear Rec.709) * texture (sRGB): a color3 shader input.
    void colorInput(const mx::NodePtr& shader, const std::string& input, const std::vector<double>& factor,
                    const TextureRef& ref, const std::string& purpose)
    {
        const mx::Color3 color{float(factor[0]), float(factor[1]), float(factor[2])};
        if (mx::NodePtr lookup = image(ref, "color3", true, purpose)) {
            mx::NodePtr value = lookup;
            if (color != mx::Color3(1.0f)) {
                mx::NodePtr scaled = node("multiply", "color3", input);
                connect(scaled, "in1", "color3", lookup);
                scaled->setInputValue("in2", color)->setColorSpace(kLinearSpace);
                value = scaled;
            }
            connect(shader, input, "color3", value);
        } else {
            shader->setInputValue(input, color)->setColorSpace(kLinearSpace);
        }
    }

    void normalInput(const mx::NodePtr& shader, const std::string& input, const TextureRef& ref,
                     const std::string& purpose)
    {
        mx::NodePtr lookup = image(ref, "vector3", false, purpose);
        if (!lookup) return;
        mx::NodePtr map = node("normalmap", "vector3", input + "_map");
        connect(map, "in", "vector3", lookup);
        if (ref.normalScale != 1.0) map->setInputValue("scale", float(ref.normalScale));
        connect(shader, input, "vector3", map);
    }

    mx::NodePtr texcoord1()
    {
        if (!m_texcoord1) {
            m_texcoord1 = node("geompropvalue", "vector2", "texcoord1");
            m_texcoord1->setInputValue("geomprop", std::string(kTexcoord1Primvar));
            m_result->usesTexcoord1 = true;
        }
        return m_texcoord1;
    }

    mx::NodePtr vertexColor()
    {
        if (!m_vertexColor) {
            m_vertexColor = node("geompropvalue", "color4", "vertex_color");
            m_vertexColor->setInputValue("geomprop", std::string(kColor0Primvar));
            m_vertexColor->setInputValue("default", mx::Color4(1.0f));
            m_result->usesVertexColor = true;
        }
        return m_vertexColor;
    }

private:
    const tinygltf::Model& m_model;
    const MaterialConversionOptions& m_options;
    MaterialConversion* m_result;
    mx::DocumentPtr m_doc;
    std::map<std::string, mx::NodePtr> m_images, m_channels;
    mx::NodePtr m_texcoord1, m_vertexColor;
};

const tinygltf::Value* Extension(const tinygltf::Material& material, const char* name)
{
    auto found = material.extensions.find(name);
    return found == material.extensions.end() ? nullptr : &found->second;
}

} // namespace

bool IsSupportedMaterialExtension(const std::string& extension)
{
    static const char* const supported[] = {
        "KHR_materials_emissive_strength", "KHR_materials_transmission", "KHR_materials_volume",
        "KHR_materials_ior", "KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_specular",
        "KHR_materials_iridescence", "KHR_materials_dispersion", "KHR_materials_unlit",
        "KHR_materials_anisotropy", "KHR_texture_transform"};
    return std::find(std::begin(supported), std::end(supported), extension) != std::end(supported);
}

std::vector<std::string> UnsupportedExtensions(const tinygltf::Model& model)
{
    // Geometry extensions the mesh loader handles (normalized and integer
    // accessors).
    static const char* const geometry[] = {"KHR_mesh_quantization"};
    std::vector<std::string> result;
    for (auto const& extension : model.extensionsUsed)
        if (!IsSupportedMaterialExtension(extension) &&
            std::find(std::begin(geometry), std::end(geometry), extension) == std::end(geometry))
            result.push_back(extension);
    return result;
}

MaterialConversion ConvertGltfMaterial(const tinygltf::Model& model, int materialIndex,
                                       const MaterialConversionOptions& options)
{
    if (materialIndex >= int(model.materials.size()))
        throw std::runtime_error("glTF material " + std::to_string(materialIndex) + " does not exist");
    // The glTF default material: all defaults (white, metallic 1, roughness 1).
    static const tinygltf::Material defaultMaterial;
    const tinygltf::Material& material =
        materialIndex >= 0 ? model.materials[size_t(materialIndex)] : defaultMaterial;
    const std::string label = materialIndex >= 0 ? "'" + material.name + "'" : "(default)";

    MaterialConversion result;
    Builder b(model, options, &result);
    mx::NodePtr shader = b.node("gltf_pbr", "surfaceshader", "gltf_shader");
    const tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;
    const bool unlit = Extension(material, "KHR_materials_unlit") != nullptr;

    // Base colour and alpha: factor (linear Rec.709) * texture (sRGB) *
    // COLOR_0. alpha_mode OPAQUE ignores alpha.
    std::vector<double> baseFactor = pbr.baseColorFactor;
    if (baseFactor.size() != 4) baseFactor = {1, 1, 1, 1};
    const int alphaMode = material.alphaMode == "MASK" ? 1 : material.alphaMode == "BLEND" ? 2 : 0;
    const mx::Color4 factor4{float(baseFactor[0]), float(baseFactor[1]), float(baseFactor[2]), float(baseFactor[3])};
    mx::NodePtr baseImage = b.image(Ref(pbr.baseColorTexture), "color4", true, "baseColorTexture");
    mx::NodePtr baseRgba;
    if (baseImage) {
        baseRgba = baseImage;
        if (factor4 != mx::Color4(1.0f)) {
            mx::NodePtr scaled = b.node("multiply", "color4", "base_color_factor");
            Builder::connect(scaled, "in1", "color4", baseImage);
            scaled->setInputValue("in2", factor4)->setColorSpace(kLinearSpace);
            baseRgba = scaled;
        }
    }
    if (options.vertexColor) {
        mx::NodePtr colored = b.node("multiply", "color4", "base_color_vertex");
        if (baseRgba) Builder::connect(colored, "in1", "color4", baseRgba);
        else colored->setInputValue("in1", factor4)->setColorSpace(kLinearSpace);
        Builder::connect(colored, "in2", "color4", b.vertexColor());
        baseRgba = colored;
    }
    mx::NodePtr baseColorNode, alphaNode;
    if (baseRgba) {
        baseColorNode = b.node("convert", "color3", "base_color");
        Builder::connect(baseColorNode, "in", "color4", baseRgba);
        if (alphaMode != 0) alphaNode = b.channel(baseRgba, 3);
    }
    auto setBaseColor = [&](const std::string& input) {
        if (baseColorNode) Builder::connect(shader, input, "color3", baseColorNode);
        else shader->setInputValue(input, mx::Color3(factor4[0], factor4[1], factor4[2]))->setColorSpace(kLinearSpace);
    };
    if (unlit) {
        // Approximation: the base colour emitted over a black, non-reflective
        // surface.
        setBaseColor("emissive");
        shader->setInputValue("base_color", mx::Color3(0.0f));
        shader->setInputValue("metallic", 0.0f);
        shader->setInputValue("specular", 0.0f);
        b.warn("KHR_materials_unlit is approximated as emission over a black surface");
    } else {
        setBaseColor("base_color");
    }
    if (alphaMode != 0) {
        shader->setInputValue("alpha_mode", alphaMode);
        if (alphaMode == 1) shader->setInputValue("alpha_cutoff", float(material.alphaCutoff));
        if (alphaNode) Builder::connect(shader, "alpha", "float", alphaNode);
        else shader->setInputValue("alpha", factor4[3]);
    }

    if (!unlit) {
        // Metallic (B) and roughness (G).
        const TextureRef mr = Ref(pbr.metallicRoughnessTexture);
        b.floatInput(shader, "metallic", pbr.metallicFactor, mr, 2, "metallicRoughnessTexture");
        b.floatInput(shader, "roughness", pbr.roughnessFactor, mr, 1, "metallicRoughnessTexture");
        b.normalInput(shader, "normal", Ref(material.normalTexture), "normalTexture");

        // Emission.
        std::vector<double> emissive = material.emissiveFactor;
        if (emissive.size() != 3) emissive = {0, 0, 0};
        const TextureRef emissiveRef = Ref(material.emissiveTexture);
        if (emissiveRef.valid() || emissive != std::vector<double>{0, 0, 0})
            b.colorInput(shader, "emissive", emissive, emissiveRef, "emissiveTexture");
    }
    if (const tinygltf::Value* ext = Extension(material, "KHR_materials_emissive_strength"))
        shader->setInputValue("emissive_strength", float(Number(*ext, "emissiveStrength", 1.0)));

    if (!unlit) {
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_ior"))
            shader->setInputValue("ior", float(Number(*ext, "ior", 1.5)));
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_transmission"))
            b.floatInput(shader, "transmission", Number(*ext, "transmissionFactor", 0), Ref(*ext, "transmissionTexture"), 0,
                         "transmissionTexture");
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_volume")) {
            b.floatInput(shader, "thickness", Number(*ext, "thicknessFactor", 0), Ref(*ext, "thicknessTexture"), 1,
                         "thicknessTexture");
            // +infinity (the glTF default) as a finite "no attenuation"
            // (DocumentLoader layers a volume only below 1e29). Thickness 0
            // means a thin-walled surface without a volume in glTF.
            const double distance = Number(*ext, "attenuationDistance", 0);
            const bool thin = Number(*ext, "thicknessFactor", 0) <= 0;
            shader->setInputValue("attenuation_distance", float(distance > 0 && !thin ? distance : 1e30));
            // A uniform input: generated colour transforms are varying, so
            // the value is converted here and left untagged.
            const auto color = Numbers(*ext, "attenuationColor", {1, 1, 1});
            std::array<float, 3> working{float(color[0]), float(color[1]), float(color[2])};
            if (options.fromLinearRec709) working = options.fromLinearRec709(working);
            shader->setInputValue("attenuation_color", mx::Color3(working[0], working[1], working[2]));
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_specular")) {
            b.floatInput(shader, "specular", Number(*ext, "specularFactor", 1), Ref(*ext, "specularTexture"), 3,
                         "specularTexture");
            b.colorInput(shader, "specular_color", Numbers(*ext, "specularColorFactor", {1, 1, 1}),
                         Ref(*ext, "specularColorTexture"), "specularColorTexture");
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_clearcoat")) {
            b.floatInput(shader, "clearcoat", Number(*ext, "clearcoatFactor", 0), Ref(*ext, "clearcoatTexture"), 0,
                         "clearcoatTexture");
            b.floatInput(shader, "clearcoat_roughness", Number(*ext, "clearcoatRoughnessFactor", 0),
                         Ref(*ext, "clearcoatRoughnessTexture"), 1, "clearcoatRoughnessTexture");
            b.normalInput(shader, "clearcoat_normal", Ref(*ext, "clearcoatNormalTexture"), "clearcoatNormalTexture");
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_sheen")) {
            b.colorInput(shader, "sheen_color", Numbers(*ext, "sheenColorFactor", {0, 0, 0}),
                         Ref(*ext, "sheenColorTexture"), "sheenColorTexture");
            b.floatInput(shader, "sheen_roughness", Number(*ext, "sheenRoughnessFactor", 0),
                         Ref(*ext, "sheenRoughnessTexture"), 3, "sheenRoughnessTexture");
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_iridescence")) {
            b.floatInput(shader, "iridescence", Number(*ext, "iridescenceFactor", 0), Ref(*ext, "iridescenceTexture"), 0,
                         "iridescenceTexture");
            shader->setInputValue("iridescence_ior", float(Number(*ext, "iridescenceIor", 1.3)));
            const double minimum = Number(*ext, "iridescenceThicknessMinimum", 100);
            const double maximum = Number(*ext, "iridescenceThicknessMaximum", 400);
            mx::NodePtr lookup =
                b.image(Ref(*ext, "iridescenceThicknessTexture"), "vector3", false, "iridescenceThicknessTexture");
            if (lookup) {
                // min + (max - min) * G
                mx::NodePtr thickness = b.node("mix", "float", "iridescence_thickness");
                thickness->setInputValue("bg", float(minimum));
                thickness->setInputValue("fg", float(maximum));
                Builder::connect(thickness, "mix", "float", b.channel(lookup, 1));
                Builder::connect(shader, "iridescence_thickness", "float", thickness);
            } else {
                shader->setInputValue("iridescence_thickness", float(maximum));
            }
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_anisotropy")) {
            // strength = factor * B; rotation = factor + atan2(2G - 1, 2R - 1)
            // (the direction in the tangent frame), as MaterialX's
            // gltf_anisotropy_image computes it.
            const double strength = Number(*ext, "anisotropyStrength", 0);
            const double rotation = Number(*ext, "anisotropyRotation", 0);
            b.floatInput(shader, "anisotropy_strength", strength, Ref(*ext, "anisotropyTexture"), 2,
                         "anisotropyTexture");
            if (mx::NodePtr lookup = b.image(Ref(*ext, "anisotropyTexture"), "vector3", false, "anisotropyTexture")) {
                auto signedChannel = [&](int index) {
                    mx::NodePtr scaled = b.node("multiply", "float", "anisotropy_direction");
                    Builder::connect(scaled, "in1", "float", b.channel(lookup, index));
                    scaled->setInputValue("in2", 2.0f);
                    mx::NodePtr centred = b.node("subtract", "float", "anisotropy_direction");
                    Builder::connect(centred, "in1", "float", scaled);
                    centred->setInputValue("in2", 1.0f);
                    return centred;
                };
                mx::NodePtr angle = b.node("atan2", "float", "anisotropy_angle");
                Builder::connect(angle, "iny", "float", signedChannel(1));
                Builder::connect(angle, "inx", "float", signedChannel(0));
                mx::NodePtr total = b.node("add", "float", "anisotropy_rotation");
                Builder::connect(total, "in1", "float", angle);
                total->setInputValue("in2", float(rotation));
                Builder::connect(shader, "anisotropy_rotation", "float", total);
            } else {
                shader->setInputValue("anisotropy_rotation", float(rotation));
            }
        }
        if (const tinygltf::Value* ext = Extension(material, "KHR_materials_dispersion"))
            shader->setInputValue("dispersion", float(Number(*ext, "dispersion", 0)));
    }

    for (auto const& [name, value] : material.extensions)
        if (!IsSupportedMaterialExtension(name))
            b.warn("material extension " + name + " is not supported; ignored");

    mx::NodePtr surface = b.doc()->addMaterialNode(kMaterialName, shader);
    (void)surface;
    for (auto& warning : result.warnings) warning = "material " + label + ": " + warning;
    return result;
}

} // namespace prism::gltf
