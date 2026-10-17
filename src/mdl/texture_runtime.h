#pragma once

#include "../texture_service.h"

#include <mi/mdl_sdk.h>

#include <map>
#include <memory>
#include <atomic>
#include <utility>
#include <vector>
#include <filesystem>
#include <mutex>
#include <tuple>
#include <string>

namespace mdl {

class NativeTextureRuntime {
public:
    struct ArgumentResourcePreflight {
        // TextureService indexes prepared resources weakly. Retain successful
        // preparation until target translation and runtime construction have
        // consumed the same entries.
        std::vector<std::shared_ptr<const prism::TextureResource>> retained;
    };
    struct Handler : mi::neuraylib::Texture_handler_deriv_base {
        NativeTextureRuntime const *owner = nullptr;
    };
    static std::shared_ptr<NativeTextureRuntime> Create(
        const mi::neuraylib::ITarget_code *code,
        mi::neuraylib::ITransaction *transaction,
        std::string *fallbackReason = nullptr);
    static bool PreflightArgumentResource(
        const mi::neuraylib::IValue_resource* resource,
        mi::neuraylib::ITransaction* transaction,
        ArgumentResourcePreflight& retained,
        std::string* fallbackReason = nullptr);
    std::shared_ptr<NativeTextureRuntime> clone() const;
    mi::Uint32 registerTexture(const mi::neuraylib::IValue_resource *resource,
                               mi::neuraylib::ITransaction *transaction);
    // Exercises the exact native callback path without requiring an MDL JIT.
    static std::shared_ptr<NativeTextureRuntime> CreateForTesting(
        const std::filesystem::path& image);
    static std::shared_ptr<NativeTextureRuntime> CreateDfDataForTesting(
        mi::neuraylib::Df_data_kind kind, mi::Size width, mi::Size height,
        mi::Size depth, const char* pixelType, const float* data);
    void lookupForTesting(float s,float t,float dsdx,float dtdx,
                          float dsdy,float dtdy,float out[4]) const;
    void lookupForTesting(float s,float t,float dsdx,float dtdx,
                          float dsdy,float dtdy,float out[4],
                          mi::neuraylib::Tex_wrap_mode wrapS,
                          mi::neuraylib::Tex_wrap_mode wrapT,
                          float const cropS[2],float const cropT[2]) const;
    void lookup3dForTesting(float s,float t,float r,float out[4],
                            mi::neuraylib::Tex_wrap_mode wrapS,
                            mi::neuraylib::Tex_wrap_mode wrapT,
                            mi::neuraylib::Tex_wrap_mode wrapR,
                            float const cropS[2],float const cropT[2],
                            float const cropR[2]) const;

    // Read-only view of one entry of the resource table (index 0 is the
    // invalid resource), for render devices that upload the same resources.
    struct ResourceView {
        bool tiled = false;
        // UDIM tile (u, v) -> prepared image; a single (0, 0) tile otherwise.
        std::vector<std::pair<std::pair<int,int>,
                              std::shared_ptr<const prism::TextureResource>>> tiles;
        // BSDF data table, when the resource is one; values are retained by owner.
        std::shared_ptr<const void> owner;
        const float* values = nullptr;
        size_t width = 0, height = 0, depth = 0, components = 0;
    };
    size_t resourceCount() const { return m_textures.size(); }
    ResourceView resource(size_t index) const;
    // Prepares the 2D texture (or UDIM set) stored under a database name, as
    // the runtime does for the textures of its own target code.
    static bool PrepareDatabaseTexture(const char* dbName,
        mi::neuraylib::ITransaction* transaction, ResourceView* result,
        std::string* error = nullptr);

    mi::neuraylib::Texture_handler_base *handler() const {
        return const_cast<mi::neuraylib::Texture_handler_base *>(
            reinterpret_cast<mi::neuraylib::Texture_handler_base const *>(&m_handler));
    }

private:
    struct DfData {
        mi::neuraylib::Df_data_kind kind = mi::neuraylib::DFK_INVALID;
        size_t width = 0, height = 0, depth = 0, components = 0;
        std::string pixelType;
        std::vector<float> values;
    };
    struct Texture {
        bool tiled = false;
        std::map<std::pair<int,int>,
                 std::shared_ptr<const prism::TextureResource>> tiles;
        std::shared_ptr<const DfData> dfData;
    };
    static bool prepareImage(const mi::neuraylib::ITexture* texture,
        const mi::neuraylib::IImage* image,
        const std::shared_ptr<prism::TextureService>& service,
        Texture& result, std::string* fallbackReason);
    NativeTextureRuntime();
    std::shared_ptr<const prism::TextureResource> croppedResource(
        std::shared_ptr<const prism::TextureResource> const& source,
        float const cropS[2],float const cropT[2]) const;
    static mi::neuraylib::Texture_handler_deriv_vtable const &Vtable();
    static std::shared_ptr<const DfData> CopyDfData(
        mi::neuraylib::Df_data_kind kind, mi::Size width, mi::Size height,
        mi::Size depth, const char* pixelType, const float* data,
        std::string* error);

    Handler m_handler{};
    std::shared_ptr<prism::TextureService> m_service;
    std::vector<Texture> m_textures; // index 0 is invalid
    mutable std::atomic<bool> m_reportedLookupFailure{false};
    // Most graphs use a single constant crop. Keep a bounded set of prepared
    // crop views; no source pixel arrays survive preparation.
    using CropKey = std::tuple<const prism::TextureResource*,int,int,int,int>;
    struct CropEntry {
        std::shared_ptr<const prism::TextureResource> resource;
        uint64_t lastUse = 0;
    };
    mutable std::mutex m_cropMutex;
    mutable std::map<CropKey,CropEntry> m_crops;
    mutable uint64_t m_cropClock = 0;
};

} // namespace mdl
