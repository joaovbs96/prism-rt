#include "color_management.h"

#include <OpenColorIO/OpenColorIO.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace OCIO = OCIO_NAMESPACE;

namespace prism::color {

namespace {

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Exact processors: OCIO's default optimization approximates pow/log/exp,
// which would move legacy gamma outputs by a code value.
constexpr OCIO::OptimizationFlags kOptimization = OCIO::OPTIMIZATION_LOSSLESS;

OCIO::ConstCPUProcessorRcPtr CpuProcessor(OCIO::ConstProcessorRcPtr const &processor)
{
    return processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, kOptimization);
}

// Columns of the linear map `processor` applies (rows of the result are the
// output channels).
Matrix3 MatrixOf(OCIO::ConstCPUProcessorRcPtr const &processor)
{
    Matrix3 m{};
    for (int column = 0; column < 3; ++column) {
        float rgb[3] = {column == 0 ? 1.0f : 0.0f, column == 1 ? 1.0f : 0.0f, column == 2 ? 1.0f : 0.0f};
        processor->applyRGB(rgb);
        for (int row = 0; row < 3; ++row) m[row * 3 + column] = rgb[row];
    }
    // A matrix reproduces the processor (the working space is linear).
    float probe[3] = {0.25f, 2.0f, 0.5f};
    const float expected[3] = {
        m[0] * probe[0] + m[1] * probe[1] + m[2] * probe[2],
        m[3] * probe[0] + m[4] * probe[1] + m[5] * probe[2],
        m[6] * probe[0] + m[7] * probe[1] + m[8] * probe[2]};
    processor->applyRGB(probe);
    for (int i = 0; i < 3; ++i)
        if (!(std::abs(probe[i] - expected[i]) <= 1e-4f * std::max(1.0f, std::abs(expected[i]))))
            throw std::runtime_error("colour conversion from linear Rec.709 is not a matrix");
    return m;
}

// MaterialX's names for the linear spaces it has transforms for (cmlib).
std::string MaterialXName(OCIO::ConstColorSpaceRcPtr const &space)
{
    static const std::map<std::string, std::string> known = {
        {"acescg", "acescg"}, {"lin_ap1", "acescg"}, {"lin_ap1_scene", "acescg"},
        {"lin_rec709", "lin_rec709"}, {"lin_rec709_scene", "lin_rec709"},
        {"lin_adobergb", "lin_adobergb"}, {"lin_adobergb_scene", "lin_adobergb"},
        {"lin_displayp3", "lin_displayp3"}, {"lin_p3d65_scene", "lin_displayp3"}};
    std::vector<std::string> names{Lower(space->getName())};
    for (size_t i = 0; i < space->getNumAliases(); ++i) names.push_back(Lower(space->getAlias(i)));
    for (auto const &name : names)
        if (auto found = known.find(name); found != known.end()) return found->second;
    return "prism_working";
}

std::string Digits(Matrix3 const &m)
{
    std::string text;
    char buffer[32];
    for (float v : m) {
        std::snprintf(buffer, sizeof(buffer), "%.9g,", v);
        text += buffer;
    }
    return text;
}

// $OCIO reaches here through RendererOptions::FromEnvironment.
std::string LoadedConfig(std::string const &requested)
{
    return requested.empty() ? kDefaultConfig : requested;
}

std::atomic<bool> g_defaultConfigUnavailable{false};

// Loads `path`. kDefaultConfig (ocio://studio-config-latest) is the one name
// that falls back: an OpenColorIO without it (2.2 has only versioned names)
// gets its newest built-in studio config, with a message in *note.
OCIO::ConstConfigRcPtr LoadConfig(std::string const &path, std::string *resolved, std::string *note)
{
    if (resolved) *resolved = path;
    const bool isDefault = path == kDefaultConfig;
    try {
        if (isDefault && g_defaultConfigUnavailable.load())
            throw OCIO::Exception("Could not find 'studio-config-latest' in the built-in configurations (forced)");
        return OCIO::Config::CreateFromFile(path.c_str());
    } catch (OCIO::Exception const &) {
        if (!isDefault) throw;
        const std::string pick = SelectBuiltinStudioConfig(BuiltinConfigNames());
        if (pick.empty()) throw;
        const std::string uri = "ocio://" + pick;
        auto config = OCIO::Config::CreateFromFile(uri.c_str());
        if (resolved) *resolved = uri;
        if (note)
            *note = "OpenColorIO " + std::string(OCIO::GetVersion()) + " has no built-in config '" +
                    std::string(kDefaultConfig) + "'; using its newest built-in studio config (" + uri + ")";
        return config;
    }
}

// Names configs give linear Rec.709 (the studio config's name and ASWF
// interop alias, ACES 1.x and other configs' names); OCIO also resolves the
// config's own aliases of each.
constexpr const char *kRec709Names[] = {
    "lin_rec709", "lin_rec709_scene", "Linear Rec.709 (sRGB)", "lin_rec709_srgb", "lin_srgb",
    "srgb_linear", "linear_srgb", "Linear sRGB", "Utility - Linear - sRGB", "scene-linear Rec.709-sRGB",
    "Linear BT.709"};

bool IsRec709Name(std::string const &space)
{
    const std::string lower = Lower(space);
    for (const char *name : kRec709Names)
        if (lower == Lower(name)) return true;
    return false;
}

OCIO::ConstColorSpaceRcPtr FindRec709(OCIO::ConstConfigRcPtr const &config)
{
    for (const char *name : kRec709Names)
        if (auto space = config->getColorSpace(name); space && !space->isData()) return space;
    return nullptr;
}

bool DataToken(std::string const &token)
{
    return token == "data" || token == "raw" || token == "identity";
}

// What pass-through colour management (no config, linear Rec.709 working
// space) can do for a texture or value tagged with `space`: the transfer
// functions of Rec.709-primaries spaces are analytic; other primaries need a
// config.
enum class PassThroughKind { None, Srgb, Gamma22, Unsupported };

PassThroughKind ClassifyPassThrough(std::string const &space)
{
    static const char *const srgb[] = {"srgb_texture", "srgb_tx", "srgb_rec709", "srgb_rec709_scene", "srgb",
                                       "srgb - texture", "utility - srgb - texture", "srgb encoded rec.709 (srgb)"};
    static const char *const gamma22[] = {"g22_rec709", "gamma 2.2 rec.709 - texture",
                                          "utility - gamma 2.2 - rec.709 - texture", "gamma22_rec709"};
    if (space.empty() || DataToken(space) || IsRec709Name(space)) return PassThroughKind::None;
    const std::string lower = Lower(space);
    for (const char *name : srgb)
        if (lower == name) return PassThroughKind::Srgb;
    for (const char *name : gamma22)
        if (lower == name) return PassThroughKind::Gamma22;
    return PassThroughKind::Unsupported;
}

float DecodeSrgb(float v)
{
    const float a = std::abs(v);
    const float linear = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return v < 0.0f ? -linear : linear;
}

float DecodeGamma22(float v)
{
    const float linear = std::pow(std::abs(v), 2.2f);
    return v < 0.0f ? -linear : linear;
}

std::string RenderingSpace(OCIO::ConstConfigRcPtr const &config, std::string const &token, std::string *warning)
{
    if (warning) warning->clear();
    if (token.empty() || token == "unknown") return {};
    // No source colour transforms: colours are used as authored, which USD
    // authors in linear Rec.709 when untagged.
    OCIO::ConstColorSpaceRcPtr space;
    if (DataToken(token) || IsRec709Name(token)) space = FindRec709(config);
    if (!space) space = config->getColorSpace(token.c_str());
    if (space && !space->isData() && config->isColorSpaceLinear(space->getName(), OCIO::REFERENCE_SPACE_SCENE))
        return space->getName();
    if (warning)
        *warning = "renderingColorSpace '" + token + "' is not a scene-linear colour space of the OpenColorIO "
                   "config; using the default working space";
    return {};
}

} // namespace

std::vector<std::string> BuiltinConfigNames()
{
    std::vector<std::string> names;
    try {
        auto const &registry = OCIO::BuiltinConfigRegistry::Get();
        for (size_t i = 0; i < registry.getNumBuiltinConfigs(); ++i) names.push_back(registry.getBuiltinConfigName(i));
    } catch (std::exception const &) {
    }
    return names;
}

// "studio-config-v4.0.0_aces-v2.0_ocio-v2.5" -> (4,0,0,2,0,2,5): the numbers in
// order, so a newer config (then ACES, then OpenColorIO) compares greater.
std::string SelectBuiltinStudioConfig(std::vector<std::string> const &names)
{
    constexpr std::string_view prefix = "studio-config-v";
    std::string best;
    std::vector<long> bestKey;
    for (auto const &name : names) {
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        std::vector<long> key;
        for (size_t i = 0; i < name.size();) {
            if (std::isdigit(static_cast<unsigned char>(name[i]))) {
                long value = 0;
                while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])))
                    value = value * 10 + (name[i++] - '0');
                key.push_back(value);
            } else {
                ++i;
            }
        }
        if (key.empty()) continue;
        if (best.empty() || key > bestKey || (key == bestKey && name > best)) {
            best = name;
            bestKey = std::move(key);
        }
    }
    return best;
}

void SetDefaultConfigUnavailableForTesting(bool unavailable) { g_defaultConfigUnavailable = unavailable; }

std::string RenderingColorSpaceWorkingSpace(std::string const &token, std::string const &config, std::string *warning)
{
    const std::string path = LoadedConfig(config);
    try {
        return RenderingSpace(LoadConfig(path, nullptr, nullptr), token, warning);
    } catch (OCIO::Exception const &error) {
        throw std::runtime_error("cannot load the OpenColorIO config '" + path + "': " + error.what());
    }
}

struct ColorManagement::Impl {
    Options requested;
    std::string config, configName, workingSpace, materialXSpace, display, view, identity;
    bool rec709 = false;
    Matrix3 fromRec709 = kIdentity, toRec709 = kIdentity;
    std::array<float, 3> luminance = kRec709Luminance;
    OCIO::ConstConfigRcPtr ocio;
    OCIO::ConstCPUProcessorRcPtr viewProcessor;
    // What Build changed from the request (config, display, view), for the caller to report.
    std::vector<std::string> notes;
    // The config's linear Rec.709.
    std::string rec709Name = kLightColorSpace;
    bool passThrough = false;
    mutable std::mutex mutex;
    mutable std::set<std::string> warnedPassThrough;
    mutable std::map<std::string, OCIO::ConstCPUProcessorRcPtr> toWorking;
};

ColorManagement::ColorManagement(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}

namespace {

std::shared_ptr<ColorManagement::Impl> Build(Options const &options, bool buildView);

} // namespace

std::shared_ptr<const ColorManagement> ColorManagement::Create(Options const &options, bool view)
{
    auto impl = Build(options, view);
    for (auto const &note : impl->notes) std::cerr << "PrismRT: " << note << std::endl;
    return std::make_shared<const ColorManagement>(std::move(impl));
}

std::shared_ptr<const ColorManagement> ColorManagement::PassThrough(Options const &requested)
{
    auto impl = std::make_shared<Impl>();
    impl->requested = requested;
    impl->passThrough = true;
    impl->workingSpace = kLightColorSpace;
    impl->materialXSpace = "lin_rec709";
    impl->rec709 = true;
    impl->display = requested.display;
    impl->view = requested.view;
    impl->identity = "pass-through|lin_rec709|";
    return std::make_shared<const ColorManagement>(std::move(impl));
}

std::shared_ptr<const ColorManagement> ColorManagement::CreateOrFallback(
    Options const &options, bool view, std::function<void(std::string const &)> const &warn)
{
    // Diagnostic switch: the state a host without a usable OpenColorIO config
    // is in (tests and renders of pass-through colour management).
    if (const char *forced = std::getenv("PRISM_COLOR_PASS_THROUGH"); forced && std::string(forced) != "0") {
        if (warn) warn("PRISM_COLOR_PASS_THROUGH is set; colour management is pass-through linear Rec.709");
        return PassThrough(options);
    }
    std::vector<Options> attempts{options};
    Options builtIn = options;
    builtIn.config = kDefaultConfig;
    if (LoadedConfig(options.config) != kDefaultConfig) attempts.push_back(builtIn);
    Options defaults;
    defaults.config = kDefaultConfig;
    if (!(defaults == builtIn)) attempts.push_back(defaults);
    for (size_t i = 0; i < attempts.size(); ++i) {
        try {
            auto impl = Build(attempts[i], view);
            impl->requested = options;
            if (warn)
                for (auto const &note : impl->notes) warn(note);
            return std::make_shared<const ColorManagement>(std::move(impl));
        } catch (std::exception const &error) {
            if (warn) {
                const bool last = i + 1 == attempts.size();
                const bool sameWorking = i + 1 < attempts.size() &&
                    attempts[i + 1].workingSpace == attempts[i].workingSpace &&
                    attempts[i + 1].renderingColorSpace == attempts[i].renderingColorSpace;
                warn(std::string(error.what()) +
                     (last ? "; colour management falls back to pass-through linear Rec.709 (no conversions)"
                           : sameWorking ? "; using the built-in OpenColorIO config (" + std::string(kDefaultConfig) + ")"
                                         : "; using the built-in OpenColorIO config's defaults"));
            }
        }
    }
    return PassThrough(options);
}

namespace {

std::shared_ptr<ColorManagement::Impl> Build(Options const &options, bool buildView)
try {
    auto impl = std::make_shared<ColorManagement::Impl>();
    impl->requested = options;
    impl->config = LoadedConfig(options.config);
    try {
        std::string note;
        impl->ocio = LoadConfig(impl->config, &impl->config, &note);
        if (!note.empty()) impl->notes.push_back(note);
    } catch (OCIO::Exception const &error) {
        throw std::runtime_error("cannot load the OpenColorIO config '" + impl->config + "': " + error.what());
    }
    auto const &config = impl->ocio;
    impl->configName = config->getName() ? config->getName() : "";

    std::string working = options.workingSpace;
    if (working.empty() && !options.renderingColorSpace.empty()) {
        std::string warning;
        working = RenderingSpace(config, options.renderingColorSpace, &warning);
        if (!warning.empty()) std::cerr << "PrismRT: " << warning << std::endl;
    }
    if (working.empty()) {
        if (config->getColorSpace(kDefaultWorkingSpace)) working = kDefaultWorkingSpace;
        else if (auto role = config->getColorSpace(OCIO::ROLE_SCENE_LINEAR)) working = role->getName();
    }
    auto space = working.empty() ? nullptr : config->getColorSpace(working.c_str());
    if (!space && IsRec709Name(working)) space = FindRec709(config);
    if (!space)
        throw std::runtime_error("the OpenColorIO config '" + impl->config +
                                 "' has no colour space '" + working + "' for the working space");
    impl->workingSpace = space->getName();
    if (space->isData() || !config->isColorSpaceLinear(impl->workingSpace.c_str(), OCIO::REFERENCE_SPACE_SCENE))
        throw std::runtime_error("the working space must be scene-linear: " + impl->workingSpace);
    impl->materialXSpace = MaterialXName(space);

    auto rec709 = FindRec709(config);
    if (!rec709)
        throw std::runtime_error("the OpenColorIO config '" + impl->config +
                                 "' has no linear Rec.709 space (lin_rec709 or a known alias)");
    impl->rec709Name = rec709->getName();
    impl->rec709 =std::string(rec709->getName()) == impl->workingSpace;
    if (!impl->rec709) {
        impl->fromRec709 = MatrixOf(CpuProcessor(config->getProcessor(rec709->getName(), impl->workingSpace.c_str())));
        impl->toRec709 = MatrixOf(CpuProcessor(config->getProcessor(impl->workingSpace.c_str(), rec709->getName())));
        // Y of the working space through Rec.709: the same weights for the
        // same colour whichever space holds it.
        for (int column = 0; column < 3; ++column) {
            float y = 0.0f;
            for (int row = 0; row < 3; ++row) y += kRec709Luminance[row] * impl->toRec709[row * 3 + column];
            impl->luminance[column] = y;
        }
    }

    impl->identity = impl->configName + "|" + impl->workingSpace + "|" + Digits(impl->fromRec709);
    if (impl->materialXSpace == "prism_working") {
        // MaterialX caches node implementations by name: one name per matrix.
        uint32_t hash = 2166136261u;
        for (unsigned char c : Digits(impl->fromRec709)) hash = (hash ^ c) * 16777619u;
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%08x", hash);
        impl->materialXSpace += suffix;
    }
    if (!buildView) {
        // Recorded only (the host applies its own view).
        impl->display = options.display;
        impl->view = options.view;
        return impl;
    }
    // A display or view the loaded config lacks (the ACES 2.0 view in an
    // older config, say) falls back to that config's default, with a note.
    impl->display = options.display.empty() ? config->getDefaultDisplay() : options.display;
    bool displayFound = false;
    for (int i = 0; i < config->getNumDisplays(); ++i) displayFound |= impl->display == config->getDisplay(i);
    if (!displayFound) {
        if (config->getNumDisplays() == 0)
            throw std::runtime_error("the OpenColorIO config '" + impl->config + "' has no displays");
        std::string names;
        for (int i = 0; i < config->getNumDisplays(); ++i) names += std::string(i ? ", " : "") + config->getDisplay(i);
        const std::string requestedDisplay = impl->display;
        impl->display = config->getDefaultDisplay();
        impl->notes.push_back("unknown OpenColorIO display '" + requestedDisplay + "' (" + names +
                              "); using the config's default display '" + impl->display + "'");
    }
    impl->view = options.view.empty() ? config->getDefaultView(impl->display.c_str()) : options.view;
    bool viewFound = false;
    for (int i = 0; i < config->getNumViews(impl->display.c_str()); ++i)
        viewFound |= impl->view == config->getView(impl->display.c_str(), i);
    if (!viewFound) {
        if (config->getNumViews(impl->display.c_str()) == 0)
            throw std::runtime_error("the OpenColorIO display '" + impl->display + "' has no views");
        std::string names;
        for (int i = 0; i < config->getNumViews(impl->display.c_str()); ++i)
            names += std::string(i ? ", " : "") + config->getView(impl->display.c_str(), i);
        const std::string requestedView = impl->view;
        impl->view = config->getDefaultView(impl->display.c_str());
        impl->notes.push_back("unknown view '" + requestedView + "' of display '" + impl->display + "' (" + names +
                              "); using the display's default view '" + impl->view + "'");
    }
    auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc(impl->workingSpace.c_str());
    transform->setDisplay(impl->display.c_str());
    transform->setView(impl->view.c_str());
    try {
        impl->viewProcessor = CpuProcessor(config->getProcessor(transform));
    } catch (OCIO::Exception const &error) {
        throw std::runtime_error("cannot build the view '" + impl->view + "': " + error.what());
    }
    return impl;
} catch (OCIO::Exception const &error) {
    // Any other OpenColorIO failure (a malformed config, a processor).
    throw std::runtime_error(std::string("OpenColorIO: ") + error.what());
}

} // namespace

bool ColorManagement::passThrough() const noexcept { return m_impl->passThrough; }
std::string const &ColorManagement::rec709Space() const noexcept { return m_impl->rec709Name; }

bool ColorManagement::OutputMatrix(std::string const &space, Matrix3 *matrix, std::string *name,
                                   std::string *error) const
{
    auto fail = [&](std::string const &message) {
        if (error) *error = message;
        return false;
    };
    const bool rec709 = space.empty() || DataToken(space) || IsRec709Name(space);
    if (m_impl->passThrough) {
        if (!rec709) return fail("'" + space + "' is not available without an OpenColorIO config");
        if (matrix) *matrix = kIdentity;
        if (name) *name = m_impl->workingSpace;
        return true;
    }
    try {
        auto const &config = m_impl->ocio;
        OCIO::ConstColorSpaceRcPtr found = rec709 ? config->getColorSpace(m_impl->rec709Name.c_str())
                                                  : config->getColorSpace(space.c_str());
        if (!found || found->isData() || !config->isColorSpaceLinear(found->getName(), OCIO::REFERENCE_SPACE_SCENE))
            return fail("'" + space + "' is not a scene-linear colour space of the OpenColorIO config");
        const std::string resolved = found->getName();
        Matrix3 m = kIdentity;
        if (resolved == m_impl->workingSpace) m = kIdentity;
        else if (resolved == m_impl->rec709Name) m = m_impl->toRec709;
        else m = MatrixOf(CpuProcessor(config->getProcessor(m_impl->workingSpace.c_str(), resolved.c_str())));
        if (matrix) *matrix = m;
        if (name) *name = resolved;
        return true;
    } catch (std::exception const &failure) {
        return fail("'" + space + "': " + failure.what());
    }
}

Options const &ColorManagement::requested() const noexcept { return m_impl->requested; }
std::string const &ColorManagement::config() const noexcept { return m_impl->config; }
std::string const &ColorManagement::configName() const noexcept { return m_impl->configName; }
std::string const &ColorManagement::workingSpace() const noexcept { return m_impl->workingSpace; }
std::string const &ColorManagement::materialXSpace() const noexcept { return m_impl->materialXSpace; }
std::string const &ColorManagement::display() const noexcept { return m_impl->display; }
std::string const &ColorManagement::view() const noexcept { return m_impl->view; }
std::string const &ColorManagement::identity() const noexcept { return m_impl->identity; }
bool ColorManagement::rec709() const noexcept { return m_impl->rec709; }
Matrix3 const &ColorManagement::fromRec709() const noexcept { return m_impl->fromRec709; }
Matrix3 const &ColorManagement::toRec709() const noexcept { return m_impl->toRec709; }
std::array<float, 3> const &ColorManagement::luminanceWeights() const noexcept { return m_impl->luminance; }

Float3 ColorManagement::FromRec709(Float3 const &c) const
{
    return m_impl->rec709 ? c : Apply(m_impl->fromRec709, c);
}

float ColorManagement::Luminance(Float3 const &c) const
{
    auto const &w = m_impl->luminance;
    return w[0] * c.x() + w[1] * c.y() + w[2] * c.z();
}

std::string ColorManagement::Resolve(std::string const &space) const
{
    if (space.empty()) return {};
    if (m_impl->passThrough) return space;
    auto found = m_impl->ocio->getColorSpace(space.c_str());
    if (found) return found->getName();
    // Linear Rec.709 under another of its names (kLightColorSpace in a config
    // without that alias).
    return IsRec709Name(space) ? m_impl->rec709Name : std::string{};
}

bool ColorManagement::NeedsConversion(std::string const &space) const
{
    if (m_impl->passThrough) {
        switch (ClassifyPassThrough(space)) {
        case PassThroughKind::Srgb:
        case PassThroughKind::Gamma22:
            return true;
        case PassThroughKind::Unsupported: {
            std::lock_guard lock(m_impl->mutex);
            if (m_impl->warnedPassThrough.insert(space).second)
                std::cerr << "PrismRT: colour space '" << space << "' needs an OpenColorIO config; without one "
                          << "(pass-through colour management) its values are used unconverted" << std::endl;
            return false;
        }
        default:
            return false;
        }
    }
    const std::string name = Resolve(space);
    if (name.empty() || name == m_impl->workingSpace) return false;
    return !m_impl->ocio->getColorSpace(name.c_str())->isData();
}

void ColorManagement::ToWorkingSpace(std::string const &space, float *pixels, size_t count, int channels) const
{
    if (!NeedsConversion(space)) {
        if (Resolve(space).empty()) throw std::runtime_error("unknown colour space: " + space);
        return;
    }
    if (channels < 3) return;
    if (m_impl->passThrough) {
        // Analytic decode to linear Rec.709, the pass-through working space.
        float (*decode)(float) = ClassifyPassThrough(space) == PassThroughKind::Srgb ? DecodeSrgb : DecodeGamma22;
        constexpr size_t kPassChunk = size_t(1) << 14;
        tbb::parallel_for(size_t(0), (count + kPassChunk - 1) / kPassChunk, [&](size_t chunk) {
            const size_t begin = chunk * kPassChunk, end = std::min(count, begin + kPassChunk);
            for (size_t p = begin; p < end; ++p)
                for (int c = 0; c < 3; ++c) pixels[p * channels + c] = decode(pixels[p * channels + c]);
        });
        return;
    }
    OCIO::ConstCPUProcessorRcPtr processor;
    {
        std::lock_guard lock(m_impl->mutex);
        auto &cached = m_impl->toWorking[Resolve(space)];
        if (!cached) cached = CpuProcessor(m_impl->ocio->getProcessor(Resolve(space).c_str(), m_impl->workingSpace.c_str()));
        processor = cached;
    }
    // Rows of a packed image: OCIO's packed descriptor takes the channel stride.
    constexpr size_t kChunk = size_t(1) << 14;
    tbb::parallel_for(size_t(0), (count + kChunk - 1) / kChunk, [&](size_t chunk) {
        const size_t begin = chunk * kChunk, n = std::min(kChunk, count - begin);
        OCIO::PackedImageDesc image(pixels + begin * channels, static_cast<long>(n), 1, 3, OCIO::BIT_DEPTH_F32,
                                    sizeof(float), static_cast<ptrdiff_t>(channels * sizeof(float)),
                                    static_cast<ptrdiff_t>(n * channels * sizeof(float)));
        processor->apply(image);
    });
}

void ColorManagement::ApplyView(float *pixels, size_t count, int channels) const
{
    if (channels < 3) return;
    constexpr size_t kChunk = size_t(1) << 14;
    tbb::parallel_for(size_t(0), (count + kChunk - 1) / kChunk, [&](size_t chunk) {
        const size_t begin = chunk * kChunk, n = std::min(kChunk, count - begin);
        OCIO::PackedImageDesc image(pixels + begin * channels, static_cast<long>(n), 1, 3, OCIO::BIT_DEPTH_F32,
                                    sizeof(float), static_cast<ptrdiff_t>(channels * sizeof(float)),
                                    static_cast<ptrdiff_t>(n * channels * sizeof(float)));
        // Without a view (Create(options, false), PassThrough) only the clamp.
        if (m_impl->viewProcessor) m_impl->viewProcessor->apply(image);
        for (size_t p = begin; p < begin + n; ++p)
            for (int c = 0; c < 3; ++c) {
                float &v = pixels[p * channels + c];
                v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
            }
    });
}

} // namespace prism::color
