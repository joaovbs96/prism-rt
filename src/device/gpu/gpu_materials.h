#pragma once

// MDL materials for GPU devices. Every distinct compiled MDL class of a scene
// is translated into one HLSL link unit, together with a generated
// dispatch function per material entry point. Materials keep the argument
// blocks of their native (CPU) programs: the HLSL backend lays
// arguments out exactly like the native backend, so only resource indices
// are rewritten into the link unit's single texture index space.

#include "../../texture_service.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class MDLMaterial;

namespace prism::device::gpu {

// One entry of the GPU texture index space used by generated MDL code.
// Index 0 is the invalid texture.
struct MdlTexture {
    enum class Kind { Invalid, Image, BsdfData };
    Kind kind = Kind::Invalid;
    // Image: UDIM tile (u, v) -> prepared image; one (0, 0) tile when untiled.
    bool tiled = false;
    std::vector<std::pair<std::pair<int, int>,
                          std::shared_ptr<const prism::TextureResource>>> tiles;
    // BsdfData: a width x height x depth table of `components` floats.
    std::shared_ptr<const void> owner;
    const float* values = nullptr;
    size_t width = 0, height = 0, depth = 0, components = 0;
};

// Per-material placement in the shared buffers.
struct MdlMaterialRecord {
    bool valid = false;
    uint32_t program = 0;          // dispatch index
    uint32_t argBlockOffset = 0;   // bytes into argumentBlocks
    std::optional<float> constantCutout;
    std::string error;
};

struct MdlShaderCode {
    bool success = false;
    std::string error;
    // Generated target code followed by the prism_mdl_* dispatch functions.
    std::string code;
    // The same code for one program at a time (callable shaders): the target
    // code without dispatch (targetCode), then programDispatch[p], whose
    // prism_mdl_* functions only reach program p, then sceneData. A compiler
    // that emits only reachable functions translates just that program.
    std::string targetCode;
    std::vector<std::string> programDispatch;
    std::string sceneData;
    std::vector<char> roData;
    std::vector<char> argumentBlocks;
    std::vector<MdlTexture> textures;
    std::vector<MdlMaterialRecord> materials; // parallel to the input
    uint32_t programCount = 0;
    int textureResults = 0;
    // String constants of the link unit (scene data names are their indices).
    std::vector<std::string> stringConstants;
    double translateSeconds = 0.0;
};

// Number of texture results cached by generated init functions. It only
// trades registers for recomputation; results do not depend on it.
constexpr int kGpuTextureResults = 16;

MdlShaderCode GenerateMdlShaderCode(std::vector<MDLMaterial const*> const& materials);

// GenerateMdlShaderCode, reusing the result for the same material bindings
// (bindings are immutable, so equal bindings give equal code and data). Scene
// edits that keep the materials then skip the MDL translation.
std::shared_ptr<const MdlShaderCode> CachedMdlShaderCode(std::vector<MDLMaterial const*> const& materials);

} // namespace prism::device::gpu
