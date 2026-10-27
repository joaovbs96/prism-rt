// The Vulkan denoiser's own passes around vk_oidn
// (src/device/vulkan/vulkan_denoiser.cpp): images are RGBA float buffers
// (16 bytes per pixel) addressed by buffer device addresses.
//   mode 0: resolve a whole-image accumulation (GpuRenderDevice::RenderImage)
//           into the colour, albedo and normal images: the colour sums (two
//           float4 per pixel: sum, sum of squares), then the light AOV sums
//           (four float4 per pixel, the albedo in the first, the normal in
//           the w of the first three), times the pixel's 1 / samples, as the
//           host reads them (gpu_device.cpp, ReadLightAovs)
//   mode 1: a normal in [-1, 1] to [0, 1] in place, for OIDN's normal
//           prefilter network, which takes it like OIDN's input process
//           (NaN to 0, clamped, then n * 0.5 + 0.5)
//   mode 2: back to [-1, 1] in place (OIDN's output process)

struct Params {
    uint64_t source;
    uint64_t scales;
    uint64_t color;
    uint64_t albedo;
    uint64_t normal;
    uint count;
    uint mode;
};

[[vk::push_constant]] Params params;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= params.count) return;
    if (params.mode == 0) {
        const float s = vk::RawBufferLoad<float>(params.scales + 4ull * i);
        const float4 c = vk::RawBufferLoad<float4>(params.source + 32ull * i);
        const uint64_t aov = params.source + 32ull * params.count + 64ull * i;
        const float4 e0 = vk::RawBufferLoad<float4>(aov);
        const float4 e1 = vk::RawBufferLoad<float4>(aov + 16ull);
        const float4 e2 = vk::RawBufferLoad<float4>(aov + 32ull);
        vk::RawBufferStore<float4>(params.color + 16ull * i, float4(c.x * s, c.y * s, c.z * s, 1.0f));
        vk::RawBufferStore<float4>(params.albedo + 16ull * i, float4(e0.x * s, e0.y * s, e0.z * s, 1.0f));
        vk::RawBufferStore<float4>(params.normal + 16ull * i, float4(e0.w * s, e1.w * s, e2.w * s, 1.0f));
        return;
    }
    const uint64_t address = params.source + 16ull * i;
    float3 v = vk::RawBufferLoad<float4>(address).xyz;
    if (params.mode == 1) {
        v = select(isnan(v), float3(0.0f, 0.0f, 0.0f), v);
        v = clamp(v, -1.0f, 1.0f) * 0.5f + 0.5f;
    } else {
        v = max(v * 2.0f - 1.0f, -1.0f);
    }
    vk::RawBufferStore<float4>(address, float4(v, 1.0f));
}
