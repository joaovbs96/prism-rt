#pragma once

// MDL materials for GPU devices. Every distinct compiled MDL class of a scene
// is translated into one HLSL link unit, together with a generated
// dispatch function per material entry point. Materials keep the argument
// blocks of their native (CPU) programs: the HLSL backend lays
// arguments out exactly like the native backend, so only resource indices
// are rewritten into the link unit's single texture index space.

#include "../../texture_service.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
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
    // Per-program layout only (zero otherwise): the program's read-only data
    // (bytes into roData), and the 32-bit words of roData where the
    // material's texture table and its program's string table start.
    uint32_t roDataOffset = 0;
    uint32_t textureTable = 0;
    uint32_t stringTable = 0;
    std::optional<float> constantCutout;
    std::string error;
};

// One MDL program translated alone with the HLSL backend: code that does not
// depend on the other programs of a scene, so its callable shader can compile
// as soon as the program's class is compiled. Its resources are local (texture
// indices, read-only data offsets and string ids start at its own zero); a
// scene places them with per-material tables (MdlShaderCode::perProgram).
struct MdlProgramCode {
    bool success = false;
    std::string error;
    // The target code (entry functions prism_p_<entry>), the prism_mdl_*
    // dispatch reaching only them and the scene data functions of its string
    // ids: what the program's callable shader inserts for its MDL code.
    std::string code;
    size_t targetCodeBytes = 0;    // the target code alone, at the start of `code`
    std::string digest;            // equal digests: interchangeable programs
    std::vector<char> roData;
    std::vector<std::string> stringConstants; // [0] unused
    // The textures of the target code, in its index order (index i + 1).
    struct Texture {
        bool image = true;         // a 2D image (database name) or a BSDF data table
        std::string name;
        std::shared_ptr<const std::vector<float>> values;
        size_t width = 0, height = 0, depth = 0, components = 0;
    };
    std::vector<Texture> textures;
    double translateSeconds = 0.0;
    bool fromDisk = false;          // read from the persistent cache (gpu-programs)
    std::string classKey;           // the class it was translated from (mdl::CompiledProgram::class_key)
};

struct MdlShaderCode {
    bool success = false;
    std::string error;
    // The per-program layout (GenerateProgramShaderCode): every program has
    // its own code (`programs`), and materials reach their resources through
    // tables in roData; `code`, `targetCode` and `programDispatch` are empty.
    bool perProgram = false;
    std::vector<std::shared_ptr<const MdlProgramCode>> programs;
    // Some material's emission varies (MDLMaterial::variesEmission): light
    // samples evaluate it, so the kernel or raygen shader is compiled with
    // PRISM_VARYING_EMITTERS (`code` starts with that define).
    bool varyingEmitters = false;
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

// The per-program layout of the materials: each distinct program's own code
// (ProgramCodes), with read-only data, texture and string tables placed per
// program and material. Cached like CachedMdlShaderCode.
MdlShaderCode GenerateProgramShaderCode(std::vector<MDLMaterial const*> const& materials);
std::shared_ptr<const MdlShaderCode> CachedProgramShaderCode(std::vector<MDLMaterial const*> const& materials);

// The code of each material's program alone (null for materials without a
// usable program), translating the programs no early translation produced, in
// parallel.
std::vector<std::shared_ptr<const MdlProgramCode>> ProgramCodes(std::vector<MDLMaterial const*> const& materials);

// Early translation: while at least one listener is registered, every
// reusable MDL class is translated on the thread that compiles it, right
// after class compilation (before its native code and textures), and each
// newly translated program is passed to every listener on that thread.
// Translations are kept for the process (and on disk where they name no
// image by database name), so later scenes and ProgramCodes reuse them.
using ProgramListener = std::function<void(std::shared_ptr<const MdlProgramCode> const&)>;
int AddProgramListener(ProgramListener listener);
void RemoveProgramListener(int token);

// An estimate, in bytes, of the MDL code a link unit of the added programs
// would give the compute kernel: their target code with the declarations
// they share counted once (by text), plus their dispatch. A single program's
// is its link unit's size; several programs' can exceed it where the SDK
// specializes shared code per program.
class LinkUnitEstimate {
public:
    void add(MdlProgramCode const& program);
    size_t bytes() const { return m_bytes; }
private:
    std::unordered_set<uint64_t> m_declarations;
    std::unordered_set<std::string> m_programs;
    size_t m_bytes = 0;
};
size_t EstimatedLinkUnitBytes(std::vector<std::shared_ptr<const MdlProgramCode>> const& programs);

} // namespace prism::device::gpu
