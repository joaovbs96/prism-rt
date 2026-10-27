// Denoising (docs/denoising.md).
//   prism_denoise_test                    the Denoiser interface on synthetic
//                                         images (OIDN's CPU device)
//   prism_denoise_test --timing W H       denoise times of a W x H frame
//   prism_denoise_test --error REF TEST MAX_RATIO [LAYER]
//       TEST's 'denoised' layer (or LAYER) is closer to REF's colour than TEST's colour
//       is: NRMSE(denoised) <= MAX_RATIO * NRMSE(colour)
//   prism_denoise_test --same A LAYER_A B LAYER_B TOLERANCE
//       two layers agree within TOLERANCE NRMSE ('-' is the beauty R, G, B)
//   prism_denoise_test --vulkan NAME_PREFIX TOLERANCE
//       the Vulkan device's denoiser (named NAME_PREFIX...) on the synthetic
//       images, within TOLERANCE NRMSE of OIDN's CPU device on the same images
//       (exit code 77: no Vulkan device)
//   prism_denoise_test --metal NAME_PREFIX TOLERANCE
//       the same for the Metal device's denoiser (exit code 77: no Metal device)

#include "../src/device/denoiser.h"
#include "../src/device/device.h"

#include <OpenImageIO/imagebuf.h>
#if PRISM_HAS_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace prism::device;

namespace {

void Require(bool condition, std::string const &message)
{
    if (!condition) throw std::runtime_error(message);
}

double Nrmse(std::vector<float> const &reference, std::vector<float> const &actual,
             size_t stride = 3, size_t referenceStride = 3)
{
    double error = 0, energy = 0;
    const size_t pixels = reference.size() / referenceStride;
    for (size_t p = 0; p < pixels; ++p)
        for (size_t c = 0; c < 3; ++c) {
            const double u = reference[p * referenceStride + c], v = actual[p * stride + c];
            error += (u - v) * (u - v);
            energy += u * u;
        }
    return std::sqrt(error / std::max(energy, 1e-20));
}

// A tiny deterministic generator (the test needs no statistical quality).
struct Random {
    uint32_t state = 12345u;
    float Next()
    {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24);
    }
};

struct Synthetic {
    unsigned width = 0, height = 0;
    std::vector<float> clean, noisy, albedo, normal;
};

// Light falling off across the image times an albedo checker, the colour
// with heavy per-pixel noise (about 1 spp of a path tracer); constant
// normals facing the camera.
Synthetic MakeSynthetic(unsigned width, unsigned height)
{
    Synthetic s;
    s.width = width;
    s.height = height;
    const size_t pixels = size_t(width) * height;
    s.clean.resize(pixels * 3);
    s.noisy.resize(pixels * 3);
    s.albedo.resize(pixels * 3);
    s.normal.resize(pixels * 3);
    Random random;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const size_t p = size_t(y) * width + x;
            const bool dark = ((x / 16) + (y / 16)) % 2 == 0;
            const float light = 0.4f + 1.6f * float(x) / float(width);
            const float noise = -std::log(std::max(1e-6f, random.Next())); // exponential, mean 1
            for (int c = 0; c < 3; ++c) {
                const float a = dark ? 0.15f + 0.05f * c : 0.8f - 0.1f * c;
                s.albedo[3 * p + c] = a;
                s.clean[3 * p + c] = a * light;
                s.noisy[3 * p + c] = a * light * noise;
                s.normal[3 * p + c] = c == 2 ? 1.0f : 0.0f;
            }
        }
    return s;
}

void UnitTests()
{
    DenoiseQuality quality{};
    for (DenoiseQuality q : {DenoiseQuality::Fast, DenoiseQuality::Balanced, DenoiseQuality::High}) {
        Require(ParseDenoiseQuality(ToString(q), &quality) && quality == q, "quality names round-trip");
    }
    Require(!ParseDenoiseQuality("best", &quality), "unknown quality accepted");
    Require(DenoiseSettings{}.quality == DenoiseQuality::High && DenoiseSettings{}.prefilter &&
                DenoiseSettings{}.hdr,
            "defaults: high quality, prefiltered guides, HDR");
    Require(DenoisingAvailable(), "this build has no denoiser");
    std::string error;
    const auto denoiser = SharedCpuDenoiser(&error);
    Require(denoiser != nullptr, "no CPU denoiser: " + error);
    Require(denoiser->Name().rfind("OIDN ", 0) == 0, "unexpected denoiser " + denoiser->Name());
    Require(SharedCpuDenoiser() == denoiser, "the CPU denoiser is not shared");
    std::cout << "Denoiser: " << denoiser->Name() << '\n';

    const Synthetic s = MakeSynthetic(256, 192);
    const double noisy = Nrmse(s.clean, s.noisy);
    for (DenoiseQuality q : {DenoiseQuality::Fast, DenoiseQuality::Balanced, DenoiseQuality::High})
        for (bool prefilter : {false, true})
            for (bool guides : {false, true}) {
                std::vector<float> output(s.noisy.size(), -1.0f);
                DenoiseRequest request;
                request.width = s.width;
                request.height = s.height;
                request.color = {s.noisy.data(), 3};
                if (guides) {
                    request.albedo = {s.albedo.data(), 3};
                    request.normal = {s.normal.data(), 3};
                }
                request.output = output.data();
                request.settings.quality = q;
                request.settings.prefilter = prefilter;
                denoiser->Denoise(request);
                const double denoised = Nrmse(s.clean, output);
                std::cout << ToString(q) << (prefilter ? " prefiltered" : "") << (guides ? " guided" : "")
                          << ": NRMSE " << noisy << " -> " << denoised << '\n';
                Require(std::isfinite(denoised) && denoised < 0.35 * noisy,
                        "denoising did not reduce the error enough");
            }

    // RGBA in place (the delegate's layout): alpha untouched; guides from an
    // interleaved buffer (stride 6), as the CLI's light AOVs.
    {
        const size_t pixels = size_t(s.width) * s.height;
        std::vector<float> rgba(pixels * 4), guides(pixels * 6), packed(pixels * 3);
        for (size_t p = 0; p < pixels; ++p) {
            for (int c = 0; c < 3; ++c) {
                rgba[4 * p + c] = s.noisy[3 * p + c];
                guides[6 * p + c] = s.albedo[3 * p + c];
                guides[6 * p + 3 + c] = s.normal[3 * p + c];
            }
            rgba[4 * p + 3] = 0.25f;
        }
        DenoiseRequest request;
        request.width = s.width;
        request.height = s.height;
        request.color = {rgba.data(), 4};
        request.albedo = {guides.data(), 6};
        request.normal = {guides.data() + 3, 6};
        request.output = rgba.data();
        request.outputStride = 4;
        denoiser->Denoise(request);
        for (size_t p = 0; p < pixels; ++p) Require(rgba[4 * p + 3] == 0.25f, "denoising changed alpha");
        // The same as packed RGB images.
        std::vector<float> reference(pixels * 3);
        DenoiseRequest packedRequest;
        packedRequest.width = s.width;
        packedRequest.height = s.height;
        packedRequest.color = {s.noisy.data(), 3};
        packedRequest.albedo = {s.albedo.data(), 3};
        packedRequest.normal = {s.normal.data(), 3};
        packedRequest.output = reference.data();
        denoiser->Denoise(packedRequest);
        Require(Nrmse(reference, rgba, 4) < 1e-5, "strided images denoise differently from packed ones");
    }

    bool threw = false;
    try {
        DenoiseRequest empty;
        denoiser->Denoise(empty);
    } catch (std::runtime_error const &) {
        threw = true;
    }
    Require(threw, "an empty request did not fail");
}

void Timing(unsigned width, unsigned height)
{
    const auto denoiser = SharedCpuDenoiser();
    Require(denoiser != nullptr, "no denoiser");
    const Synthetic s = MakeSynthetic(width, height);
    std::vector<float> output(s.noisy.size());
    for (DenoiseQuality q : {DenoiseQuality::Fast, DenoiseQuality::Balanced, DenoiseQuality::High})
        for (bool prefilter : {false, true}) {
            DenoiseRequest request;
            request.width = width;
            request.height = height;
            request.color = {s.noisy.data(), 3};
            request.albedo = {s.albedo.data(), 3};
            request.normal = {s.normal.data(), 3};
            request.output = output.data();
            request.settings.quality = q;
            request.settings.prefilter = prefilter;
            denoiser->Denoise(request); // warm-up (filter set-up)
            std::vector<double> times;
            for (int i = 0; i < 5; ++i) {
                const auto start = std::chrono::steady_clock::now();
                denoiser->Denoise(request);
                times.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            std::cout << width << "x" << height << " " << ToString(q) << (prefilter ? " prefiltered" : "")
                      << ": median " << times[2] << " ms (min " << times[0] << ", max " << times[4] << ")\n";
        }
#if PRISM_HAS_OIDN
    // OIDN alone: one filter committed once, executed repeatedly (what
    // Denoise costs without its per-call set-up).
    oidn::DeviceRef device = oidn::newDevice(oidn::DeviceType::CPU);
    device.commit();
    oidn::FilterRef filter = device.newFilter("RT");
    filter.setImage("color", const_cast<float *>(s.noisy.data()), oidn::Format::Float3, width, height);
    filter.setImage("albedo", const_cast<float *>(s.albedo.data()), oidn::Format::Float3, width, height);
    filter.setImage("normal", const_cast<float *>(s.normal.data()), oidn::Format::Float3, width, height);
    filter.setImage("output", output.data(), oidn::Format::Float3, width, height);
    filter.set("hdr", true);
    const auto commitStart = std::chrono::steady_clock::now();
    filter.commit();
    const double commit = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - commitStart).count();
    filter.execute();
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
        const auto start = std::chrono::steady_clock::now();
        filter.execute();
        times.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    std::cout << width << "x" << height << " OIDN high, execute only: median " << times[2]
              << " ms (commit " << commit << " ms)\n";
#endif
}

// The three channels of a layer of an EXR ('-' the beauty R, G, B).
std::vector<float> ReadLayer(std::string const &path, std::string const &layer, int *width, int *height)
{
    OIIO::ImageBuf image(path);
    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT))
        throw std::runtime_error("cannot read " + path + ": " + image.geterror());
    auto const &spec = image.spec();
    const std::string prefix = layer == "-" ? std::string() : layer + ".";
    int channels[3];
    for (int c = 0; c < 3; ++c) {
        channels[c] = spec.channelindex(prefix + "RGB"[c]);
        Require(channels[c] >= 0, path + " has no channel " + prefix + "RGB"[c]);
    }
    *width = spec.width;
    *height = spec.height;
    std::vector<float> pixel(spec.nchannels), result(size_t(spec.width) * spec.height * 3);
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x) {
            image.getpixel(spec.x + x, spec.y + y, pixel.data());
            for (int c = 0; c < 3; ++c) {
                const float v = pixel[channels[c]];
                Require(std::isfinite(v), "non-finite pixel in " + path);
                result[(size_t(y) * spec.width + x) * 3 + c] = v;
            }
        }
    return result;
}

// A GPU device's denoiser on host images (the delegate's path: vk_oidn on
// Vulkan, OIDN's Metal device on Metal, or OIDN's CPU device when neither
// can run), against OIDN's CPU device on the same images: every quality,
// with and without prefiltered guides.
int GpuDenoiserTests(DevicePreference preference, DeviceKind kind, std::string const &label,
                     std::string const &namePrefix, double tolerance)
{
    std::string note;
    const auto device = CreateDevice(preference, &note);
    if (device->Info().kind != kind) {
        std::cout << "No " << label << " device (" << note << "): skipped\n";
        return 77;
    }
    std::string error;
    const auto gpu = device->CreateDenoiser(&error);
    Require(gpu != nullptr, "the " + label + " device has no denoiser: " + error);
    const auto cpu = SharedCpuDenoiser(&error);
    Require(cpu != nullptr, "no CPU denoiser: " + error);
    std::cout << "Denoiser: " << gpu->Name() << '\n';
    Require(gpu->Name().rfind(namePrefix, 0) == 0, "expected a denoiser named " + namePrefix + "...");

    const Synthetic s = MakeSynthetic(256, 192);
    const double noisy = Nrmse(s.clean, s.noisy);
    double worst = 0;
    for (DenoiseQuality q : {DenoiseQuality::Fast, DenoiseQuality::Balanced, DenoiseQuality::High})
        for (bool prefilter : {false, true}) {
            std::vector<float> fromGpu(s.noisy.size(), -1.0f), fromCpu(s.noisy.size(), -1.0f);
            DenoiseRequest request;
            request.width = s.width;
            request.height = s.height;
            request.color = {s.noisy.data(), 3};
            request.albedo = {s.albedo.data(), 3};
            request.normal = {s.normal.data(), 3};
            request.settings.quality = q;
            request.settings.prefilter = prefilter;
            request.output = fromGpu.data();
            gpu->Denoise(request);
            request.output = fromCpu.data();
            cpu->Denoise(request);
            const double denoised = Nrmse(s.clean, fromGpu), agreement = Nrmse(fromCpu, fromGpu);
            std::cout << ToString(q) << (prefilter ? " prefiltered" : "") << ": NRMSE " << noisy << " -> "
                      << denoised << " (CPU device " << Nrmse(s.clean, fromCpu) << "); against the CPU device "
                      << agreement << '\n';
            Require(std::isfinite(denoised) && denoised < 0.35 * noisy, "denoising did not reduce the error enough");
            worst = std::max(worst, agreement);
        }
    std::cout << "Largest difference from the CPU device: " << worst << " (tolerance " << tolerance << ")\n";
    Require(worst <= tolerance, "the " + label + " denoiser differs from OIDN's CPU device");

    // RGBA in place with interleaved guides (the delegate's layout) at
    // another size (new filters): prefiltered albedo and normal, the albedo
    // alone, no guides.
    const Synthetic small = MakeSynthetic(100, 60);
    const size_t pixels = size_t(small.width) * small.height;
    std::vector<float> rgba(pixels * 4), guides(pixels * 6);
    for (size_t p = 0; p < pixels; ++p) {
        for (int c = 0; c < 3; ++c) {
            rgba[4 * p + c] = small.noisy[3 * p + c];
            guides[6 * p + c] = small.albedo[3 * p + c];
            guides[6 * p + 3 + c] = small.normal[3 * p + c];
        }
        rgba[4 * p + 3] = 0.25f;
    }
    for (int guide = 0; guide < 3; ++guide) {
        DenoiseRequest request;
        request.width = small.width;
        request.height = small.height;
        request.settings.prefilter = guide == 0;
        if (guide < 2) request.albedo = {guides.data(), 6};
        if (guide < 1) request.normal = {guides.data() + 3, 6};
        std::vector<float> fromCpu(pixels * 3);
        request.color = {small.noisy.data(), 3};
        request.output = fromCpu.data();
        cpu->Denoise(request);
        std::vector<float> inPlace = rgba;
        request.color = {inPlace.data(), 4};
        request.output = inPlace.data();
        request.outputStride = 4;
        gpu->Denoise(request);
        for (size_t p = 0; p < pixels; ++p) Require(inPlace[4 * p + 3] == 0.25f, "denoising changed alpha");
        const double agreement = Nrmse(fromCpu, inPlace, 4);
        std::cout << "100x60 RGBA in place, "
                  << (guide == 0 ? "prefiltered albedo and normal" : guide == 1 ? "albedo" : "no guides")
                  << ": against the CPU device " << agreement << '\n';
        Require(agreement <= tolerance, "the strided denoise differs from the CPU device's");
    }

    bool threw = false;
    try {
        DenoiseRequest empty;
        gpu->Denoise(empty);
    } catch (std::runtime_error const &) {
        threw = true;
    }
    Require(threw, "an empty request did not fail");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode.empty()) {
            UnitTests();
            std::cout << "Denoiser tests passed\n";
            return 0;
        }
        if (mode == "--timing" && argc == 4) {
            Timing(unsigned(std::stoul(argv[2])), unsigned(std::stoul(argv[3])));
            return 0;
        }
        if (mode == "--error" && (argc == 5 || argc == 6)) {
            int w0, h0, w1, h1, w2, h2;
            const auto reference = ReadLayer(argv[2], "-", &w0, &h0);
            const auto color = ReadLayer(argv[3], "-", &w1, &h1);
            const auto denoised = ReadLayer(argv[3], argc == 6 ? argv[5] : "denoised", &w2, &h2);
            Require(w0 == w1 && h0 == h1 && w1 == w2 && h1 == h2, "image sizes differ");
            const double raw = Nrmse(reference, color), filtered = Nrmse(reference, denoised);
            const double maxRatio = std::stod(argv[4]);
            std::cout << "NRMSE against the reference: colour " << raw << ", denoised " << filtered
                      << " (ratio " << filtered / raw << ", at most " << maxRatio << ")\n";
            return filtered <= maxRatio * raw ? 0 : 1;
        }
        if (mode == "--vulkan" && argc == 4) {
            const int status =
                GpuDenoiserTests(DevicePreference::Vulkan, DeviceKind::Vulkan, "Vulkan", argv[2], std::stod(argv[3]));
            if (status == 0) std::cout << "Vulkan denoiser tests passed\n";
            return status;
        }
        if (mode == "--metal" && argc == 4) {
            const int status =
                GpuDenoiserTests(DevicePreference::Metal, DeviceKind::Metal, "Metal", argv[2], std::stod(argv[3]));
            if (status == 0) std::cout << "Metal denoiser tests passed\n";
            return status;
        }
        if (mode == "--same" && argc == 7) {
            int w0, h0, w1, h1;
            const auto a = ReadLayer(argv[2], argv[3], &w0, &h0);
            const auto b = ReadLayer(argv[4], argv[5], &w1, &h1);
            Require(w0 == w1 && h0 == h1, "image sizes differ");
            const double nrmse = Nrmse(a, b), tolerance = std::stod(argv[6]);
            std::cout << "NRMSE " << nrmse << " (tolerance " << tolerance << ")\n";
            return nrmse <= tolerance ? 0 : 1;
        }
        std::cerr << "usage: prism_denoise_test [--timing W H | --error REF TEST MAX_RATIO [LAYER] |"
                     " --same A LAYER_A B LAYER_B TOLERANCE | --vulkan NAME_PREFIX TOLERANCE |"
                     " --metal NAME_PREFIX TOLERANCE]\n";
        return 2;
    } catch (std::exception const &e) {
        std::cerr << "prism_denoise_test: " << e.what() << '\n';
        return 1;
    }
}
