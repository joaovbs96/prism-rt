#include "color_management.h"

#include <OpenColorIO/OpenColorIO.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

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

} // namespace

struct ColorManagement::Impl {
    Options requested;
    std::string config, configName, workingSpace, materialXSpace, display, view, identity;
    bool rec709 = false;
    Matrix3 fromRec709 = kIdentity, toRec709 = kIdentity;
    std::array<float, 3> luminance = kRec709Luminance;
    OCIO::ConstConfigRcPtr ocio;
    OCIO::ConstCPUProcessorRcPtr viewProcessor;
    mutable std::mutex mutex;
    mutable std::map<std::string, OCIO::ConstCPUProcessorRcPtr> toWorking;
};

ColorManagement::ColorManagement(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}

std::shared_ptr<const ColorManagement> ColorManagement::Create(Options const &options)
{
    auto impl = std::make_shared<Impl>();
    impl->requested = options;
    impl->config = options.config;
    if (impl->config.empty()) {
        const char *env = std::getenv("OCIO");
        impl->config = env && *env ? env : kDefaultConfig;
    }
    try {
        impl->ocio = OCIO::Config::CreateFromFile(impl->config.c_str());
    } catch (OCIO::Exception const &error) {
        throw std::runtime_error("cannot load the OpenColorIO config '" + impl->config + "': " + error.what());
    }
    auto const &config = impl->ocio;
    impl->configName = config->getName() ? config->getName() : "";

    std::string working = options.workingSpace;
    if (working.empty()) {
        if (config->getColorSpace(kDefaultWorkingSpace)) working = kDefaultWorkingSpace;
        else if (auto role = config->getColorSpace(OCIO::ROLE_SCENE_LINEAR)) working = role->getName();
    }
    auto space = working.empty() ? nullptr : config->getColorSpace(working.c_str());
    if (!space)
        throw std::runtime_error("the OpenColorIO config '" + impl->config +
                                 "' has no colour space '" + working + "' for the working space");
    impl->workingSpace = space->getName();
    if (space->isData() || !config->isColorSpaceLinear(impl->workingSpace.c_str(), OCIO::REFERENCE_SPACE_SCENE))
        throw std::runtime_error("the working space must be scene-linear: " + impl->workingSpace);
    impl->materialXSpace = MaterialXName(space);

    auto rec709 = config->getColorSpace(kLightColorSpace);
    if (!rec709)
        throw std::runtime_error("the OpenColorIO config '" + impl->config + "' has no linear Rec.709 space (lin_rec709)");
    impl->rec709 = std::string(rec709->getName()) == impl->workingSpace;
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

    impl->display = options.display.empty() ? config->getDefaultDisplay() : options.display;
    bool displayFound = false;
    for (int i = 0; i < config->getNumDisplays(); ++i) displayFound |= impl->display == config->getDisplay(i);
    if (!displayFound) {
        std::string names;
        for (int i = 0; i < config->getNumDisplays(); ++i) names += std::string(i ? ", " : "") + config->getDisplay(i);
        throw std::runtime_error("unknown OpenColorIO display '" + impl->display + "' (" + names + ")");
    }
    impl->view = options.view.empty() ? config->getDefaultView(impl->display.c_str()) : options.view;
    bool viewFound = false;
    for (int i = 0; i < config->getNumViews(impl->display.c_str()); ++i)
        viewFound |= impl->view == config->getView(impl->display.c_str(), i);
    if (!viewFound) {
        std::string names;
        for (int i = 0; i < config->getNumViews(impl->display.c_str()); ++i)
            names += std::string(i ? ", " : "") + config->getView(impl->display.c_str(), i);
        throw std::runtime_error("unknown view '" + impl->view + "' of display '" + impl->display + "' (" + names + ")");
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
    impl->identity = impl->configName + "|" + impl->workingSpace + "|" + Digits(impl->fromRec709);
    if (impl->materialXSpace == "prism_working") {
        // MaterialX caches node implementations by name: one name per matrix.
        uint32_t hash = 2166136261u;
        for (unsigned char c : Digits(impl->fromRec709)) hash = (hash ^ c) * 16777619u;
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%08x", hash);
        impl->materialXSpace += suffix;
    }
    return std::make_shared<const ColorManagement>(std::move(impl));
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
    auto found = m_impl->ocio->getColorSpace(space.c_str());
    return found ? found->getName() : std::string{};
}

bool ColorManagement::NeedsConversion(std::string const &space) const
{
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
        m_impl->viewProcessor->apply(image);
        for (size_t p = begin; p < begin + n; ++p)
            for (int c = 0; c < 3; ++c) {
                float &v = pixels[p * channels + c];
                v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
            }
    });
}

namespace {
std::mutex gActiveMutex;
std::shared_ptr<const ColorManagement> gActive;
} // namespace

std::shared_ptr<const ColorManagement> Active()
{
    std::lock_guard lock(gActiveMutex);
    if (!gActive) gActive = ColorManagement::Create({});
    return gActive;
}

void SetActive(std::shared_ptr<const ColorManagement> colorManagement)
{
    std::lock_guard lock(gActiveMutex);
    gActive = std::move(colorManagement);
}

std::shared_ptr<const ColorManagement> Activate(Options const &options)
{
    {
        std::lock_guard lock(gActiveMutex);
        if (gActive && gActive->requested() == options) return gActive;
    }
    auto created = ColorManagement::Create(options);
    std::lock_guard lock(gActiveMutex);
    gActive = created;
    return gActive;
}

} // namespace prism::color
