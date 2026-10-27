// OIDN's CPU device behind the Denoiser interface (docs/denoising.md).

#include "denoiser.h"

#if PRISM_HAS_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

#include <mutex>
#include <stdexcept>
#include <vector>

namespace prism::device {

const char* ToString(DenoiseQuality quality)
{
    switch (quality) {
    case DenoiseQuality::Fast: return "fast";
    case DenoiseQuality::Balanced: return "balanced";
    case DenoiseQuality::High: return "high";
    }
    return "high";
}

bool ParseDenoiseQuality(std::string_view text, DenoiseQuality* quality)
{
    for (DenoiseQuality candidate : {DenoiseQuality::Fast, DenoiseQuality::Balanced, DenoiseQuality::High})
        if (text == ToString(candidate)) {
            *quality = candidate;
            return true;
        }
    return false;
}

#if PRISM_HAS_OIDN
namespace {

oidn::Quality OidnQuality(DenoiseQuality quality)
{
    switch (quality) {
    case DenoiseQuality::Fast: return oidn::Quality::Fast;
    case DenoiseQuality::Balanced: return oidn::Quality::Balanced;
    case DenoiseQuality::High: return oidn::Quality::High;
    }
    return oidn::Quality::High;
}

class OidnCpuDenoiser final : public Denoiser {
public:
    OidnCpuDenoiser()
    {
        m_device = oidn::newDevice(oidn::DeviceType::CPU);
        Check("cannot create OIDN's CPU device");
        m_device.commit();
        Check("cannot create OIDN's CPU device");
        m_color = m_device.newFilter("RT");
        Check("cannot create OIDN's RT filter");
    }

    std::string Name() const override { return "OIDN " OIDN_VERSION_STRING " CPU"; }

    void Denoise(DenoiseRequest const& request) override
    {
        std::lock_guard lock(m_mutex);
        const size_t width = request.width, height = request.height;
        if (!width || !height || !request.color.pixels || !request.output || request.color.stride < 3 ||
            request.outputStride < 3)
            throw std::runtime_error("denoise: empty or malformed images");
        // OIDN takes a normal only together with an albedo.
        const bool albedo = request.albedo.pixels != nullptr;
        const bool normal = albedo && request.normal.pixels != nullptr;
        const bool prefilter = request.settings.prefilter && albedo;
        HostImage albedoImage = request.albedo, normalImage = request.normal;
        if (prefilter) {
            // The guides, denoised in place in packed copies (the inputs stay
            // untouched), are then noise-free for the colour filter.
            albedoImage = Prefilter("albedo", request.albedo, width, height, request.settings, &m_albedo);
            if (normal)
                normalImage = Prefilter("normal", request.normal, width, height, request.settings, &m_normal);
        }
        Bind(m_color, "color", request.color, width, height);
        if (albedo) Bind(m_color, "albedo", albedoImage, width, height);
        else m_color.unsetImage("albedo");
        if (normal) Bind(m_color, "normal", normalImage, width, height);
        else m_color.unsetImage("normal");
        m_color.setImage("output", request.output, oidn::Format::Float3, width, height, 0,
                         size_t(request.outputStride) * sizeof(float),
                         size_t(request.outputStride) * sizeof(float) * width);
        m_color.set("hdr", request.settings.hdr);
        m_color.set("cleanAux", prefilter);
        m_color.set("quality", OidnQuality(request.settings.quality));
        m_color.commit();
        Check("denoise: cannot set up OIDN's filter");
        m_color.execute();
        Check("denoise: OIDN failed");
    }

private:
    void Check(const char* what)
    {
        const char* message = nullptr;
        if (m_device.getError(message) != oidn::Error::None)
            throw std::runtime_error(std::string(what) + ": " + (message ? message : "unknown error"));
    }

    static void Bind(oidn::FilterRef& filter, const char* name, HostImage image, size_t width, size_t height)
    {
        const size_t pixelBytes = size_t(image.stride) * sizeof(float);
        filter.setImage(name, const_cast<float*>(image.pixels), oidn::Format::Float3, width, height, 0,
                        pixelBytes, pixelBytes * width);
    }

    HostImage Prefilter(const char* name, HostImage image, size_t width, size_t height,
                        DenoiseSettings const& settings, std::vector<float>* scratch)
    {
        scratch->resize(width * height * 3);
        for (size_t p = 0; p < width * height; ++p)
            for (int c = 0; c < 3; ++c) (*scratch)[3 * p + c] = image.pixels[p * image.stride + c];
        oidn::FilterRef& filter = std::string_view(name) == "albedo" ? m_albedoFilter : m_normalFilter;
        if (!filter) filter = m_device.newFilter("RT");
        filter.setImage(name, scratch->data(), oidn::Format::Float3, width, height);
        filter.setImage("output", scratch->data(), oidn::Format::Float3, width, height);
        filter.set("quality", OidnQuality(settings.quality));
        filter.commit();
        Check("denoise: cannot set up OIDN's prefilter");
        filter.execute();
        Check("denoise: OIDN's prefilter failed");
        return {scratch->data(), 3};
    }

    std::mutex m_mutex;
    oidn::DeviceRef m_device;
    // Filters are kept between calls: committing one again for images of
    // the same size reuses its memory.
    oidn::FilterRef m_color = {}, m_albedoFilter = {}, m_normalFilter = {};
    std::vector<float> m_albedo, m_normal;
};

} // namespace

bool DenoisingAvailable() { return true; }

std::shared_ptr<Denoiser> SharedCpuDenoiser(std::string* error)
{
    static std::mutex mutex;
    static std::shared_ptr<Denoiser> shared;
    static std::string failure;
    std::lock_guard lock(mutex);
    if (!shared && failure.empty()) {
        try {
            shared = std::make_shared<OidnCpuDenoiser>();
        } catch (std::exception const& e) {
            failure = e.what();
        }
    }
    if (!shared && error) *error = failure;
    return shared;
}

#else

bool DenoisingAvailable() { return false; }

std::shared_ptr<Denoiser> SharedCpuDenoiser(std::string* error)
{
    if (error) *error = "this build has no denoiser (configure with Open Image Denoise; PRISM_ENABLE_OIDN)";
    return nullptr;
}

#endif

} // namespace prism::device
