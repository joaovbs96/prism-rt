#pragma once

#include "material.h"
#include "geometry.h"
#include "mdl/context.h"
#include "mdl/material_compiler.h"
#include "mdl/backend_compiler.h"
#include "mdl/texture_runtime.h"
#include "color/color_management.h"
#include "materialx/loader.h"
#include "materialx/mdl_generator.h"
#include "resource_limits.h"
#include "varying_emitter_signal.h"

#include <mi/mdl_sdk.h>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

// Element types of the MDL state's matrix and texture-result pointers. MDL SDK
// 2026 uses the aligned tct_float4_a16 here, earlier releases tct_float4.
using MdlStateFloat4 = std::remove_const_t<std::remove_pointer_t<
    decltype(mi::neuraylib::Shading_state_material_with_derivs::text_results)>>;
using MdlStateMatrixRow = std::remove_const_t<std::remove_pointer_t<
    decltype(mi::neuraylib::Shading_state_material_with_derivs::world_to_object)>>;
#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <new>

inline bool PrismMdlVerbose()
{
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

// Self-contained MDL material that takes a .mtlx file path, compiles
// it through the full MaterialX -> MDL -> native CPU pipeline, and
// provides PrismRT's Material interface for path tracing.
class MDLMaterial : public Material {
    struct ShadingStateData;
public:
    struct CompilationTimings {
        double generationSeconds = 0.0;
        // Part of generationSeconds: loading and preprocessing the document.
        double loadSeconds = 0.0;
        double classCompilationSeconds = 0.0;
        double nativeJitSeconds = 0.0;
        bool persistentGenerationCacheHit = false;
        uint64_t generationCalls = 0;
        bool templateCacheHit = false;
        bool foldedParameters = false;
    };
    // Chooses the compile specification once generation has published the
    // material's uniforms. Empty: every class parameter but strings is a runtime argument.
    using CompileSelector =
        std::function<mdl::CompileSpecification(const GeneratedMDL&)>;
    // Construct a material from an already validated immutable native program
    // and a freshly bound argument/resource block. The binding retains its
    // program and runtime, so no generator or SDK transaction is needed here.
    explicit MDLMaterial(mdl::TargetCode boundTarget)
        : m_target(std::move(boundTarget)), m_valid(m_target.success)
    {
        evaluateUniformCutout();
        evaluateUniformEmission();
        if (m_varyingEmission) prism::varying_emitters::Note();
        evaluateTransmission();
    }
    // Compiles a .mtlx file into a callable MDL material.
    //   mtlx_path:         path to the .mtlx file
    //   mdl_context:       initialized MDL SDK context (from main)
    //   mx_libraries_path: path to MaterialX install libraries/
    //   mx_mdl_path:       path to MaterialX install libraries/mdl/
    //   material_name:     name of the material node (empty = use first material found)
    //   select:            which class parameters to fold (empty = none)
    MDLMaterial(const std::string& mtlx_path,
                mdl::Context& mdl_context,
                const std::string& mx_libraries_path,
                const std::string& mx_mdl_path,
                const std::string& material_name = "",
                CompilationTimings* timings = nullptr,
                const CompileSelector& select = {})
    {
        prism::PreparationAdmission::Reservation admission(
            prism::PreparationAdmission::Shared(), prism::kMaterialCompileReservationBytes);
        if (PrismMdlVerbose())
            std::cout << "[MDLMaterial] Compiling: " << mtlx_path << "\n";

        // Phase 3: MaterialX -> MDL code generation
        // Shared across materials: constructing a generator loads the whole
        // MaterialX standard library.
        const auto generationStart = std::chrono::steady_clock::now();
        MDLGenerator& generator = *MDLGenerator::workerLocal(mx_libraries_path);
        DocumentLoader doc_loader(generator.get_stdlib(), true);
        if (auto color = prism::color::Active()) {
            doc_loader.setUniformColorConversion({color->materialXSpace(),
                [color](const std::string& space, std::array<float, 3>& rgb) {
                    // Linear Rec.709 as the glTF converter's attenuation
                    // colour (gltf_mesh.h); other spaces through OpenColorIO.
                    if (space == prism::color::kLightColorSpace) {
                        const Float3 c = color->FromRec709(Float3(rgb[0], rgb[1], rgb[2]));
                        rgb = {c.x(), c.y(), c.z()};
                        return true;
                    }
                    if (color->Resolve(space).empty()) return false;
                    color->ToWorkingSpace(space, rgb.data(), 1, 3);
                    return true;
                }});
        }

        MaterialX::DocumentPtr doc = doc_loader.load(mtlx_path);
        if (timings) timings->loadSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - generationStart).count();
        if (!doc) {
            std::cerr << "[MDLMaterial] ERROR: Failed to load " << mtlx_path << "\n";
            m_valid = false;
            return;
        }

        // Resolve material name — use provided name or find the first material node
        std::string mat_name = material_name;
        if (mat_name.empty()) {
            auto materials = doc->getMaterialNodes();
            if (materials.empty()) {
                std::cerr << "[MDLMaterial] ERROR: No material nodes found in " << mtlx_path << "\n";
                m_valid = false;
                return;
            }
            mat_name = materials[0]->getName();
            if (PrismMdlVerbose())
                std::cout << "[MDLMaterial] Auto-detected material: " << mat_name << "\n";
        }

        // Generated wrapper identifiers otherwise inherit scene material names
        // (Material_0, Material_1, ...), making equivalent standalone and
        // Hydra graphs compile as distinct MDL classes. Resolve the selected
        // node first, then give only that terminal wrapper a stable name.
        MaterialX::NodePtr selectedMaterial;
        for (const auto& material : doc->getMaterialNodes()) {
            if (material->getName() == mat_name) {
                selectedMaterial = material;
                break;
            }
        }
        if (selectedMaterial) {
            selectedMaterial->setName("PrismMaterial");
            mat_name = selectedMaterial->getName();
        }

        GeneratedMDL gen = generator.generate(doc, mat_name);
        m_generated_uniforms = gen.uniforms;
        if (timings) timings->generationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - generationStart).count();
        if (timings) timings->persistentGenerationCacheHit =
            gen.generationPersistentCacheHit;
        if (timings) {
            timings->generationCalls = gen.generationCacheHit ? 0 : 1;
            timings->templateCacheHit = gen.generationCacheHit;
        }
        if (!gen.success) {
            std::cerr << "[MDLMaterial] ERROR: MDL generation failed: " << gen.error_message << "\n";
            m_valid = false;
            return;
        }

        // Phase 4: Load MDL into SDK and compile
        const auto compilationStart = std::chrono::steady_clock::now();
        m_compiler = std::make_unique<mdl::MaterialCompiler>(mdl_context, mx_mdl_path);
        m_compiled = m_compiler->compile(gen, mtlx_path,
            select ? select(gen) : mdl::CompileSpecification{});
        m_foldedParameters = !m_compiled.folded_parameters.empty();
        if (timings) timings->classCompilationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - compilationStart).count();
        if (timings) timings->foldedParameters = m_foldedParameters;

        if (!m_compiled.success) {
            std::cerr << "[MDLMaterial] ERROR: Compilation failed: " << m_compiled.error_message << "\n";
            m_valid = false;
            return;
        }

        // Phase 5: Backend compilation to native CPU code
        mdl::BackendCompiler backend(mdl_context);
        m_target = backend.generate_target_code(m_compiled);
        if (timings) timings->nativeJitSeconds = m_target.nativeJitSeconds;

        if (!m_target.success) {
            std::cerr << "[MDLMaterial] ERROR: Backend compilation failed: " << m_target.error_message << "\n";
            m_compiled.release();
            m_valid = false;
            return;
        }

        // Native target code and its copied argument block are self-contained.
        // Commit and release the compiled DAG/transaction now instead of
        // retaining one SDK transaction for every live scene material.
        m_compiled.release();
        m_valid = true;
        evaluateUniformCutout();
        evaluateUniformEmission();
        if (m_varyingEmission) prism::varying_emitters::Note();
        evaluateTransmission();
        // NOTE: prints the *resolved* name (material_name may be empty)
        if (PrismMdlVerbose())
            std::cout << "[MDLMaterial] OK: " << m_target.program->target_code->get_callable_function_count()
                      << " functions compiled from '" << mat_name << "'\n";
    }

    ~MDLMaterial() {
        m_compiled.release();
    }

    bool is_valid() const { return m_valid; }
    bool transmissive() const override { return m_transmissive; }

    // Accessor for the target code (needed by MaterialXTest scene)
    const mdl::TargetCode& get_target_code() const { return m_target; }
    const std::vector<GeneratedUniform>& get_generated_uniforms() const {
        return m_generated_uniforms;
    }
    // True when some class parameters were baked into the program, so a
    // generated uniform missing from its parameters is a folded constant.
    bool has_folded_parameters() const { return m_foldedParameters; }

    // ----------------------------------------------------------------
    // PrismRT Material interface
    // ----------------------------------------------------------------

    BRDFSample sample(const RayHit ray_hit, unsigned int& seed) override
    { return sampleWithState(ray_hit,seed,nullptr); }

    bool prepare(const RayHit& hit, PreparedShadingContext& context) const override
    {
        static_assert(sizeof(ShadingStateData)<=sizeof(context.storage));
        static_assert(alignof(PreparedShadingContext)>=alignof(ShadingStateData));
        context.owner=this;
        context.valid=false;
        if (!m_valid || !m_target.program) return false;
        auto* ss=new(context.storage) ShadingStateData;
        build_shading_state(hit,*ss);
        context.valid=m_valid && m_target.program->target_code->execute_init(
            m_target.program->functions.init_function_index,ss->base(),m_target.texture_handler(),
            m_target.argument_data())==0;
        return context.valid;
    }

    BRDFSample samplePrepared(const RayHit hit,unsigned int& seed,
                              PreparedShadingContext& context) override
    {
        auto* ss=context.owner==this&&context.valid
            ? reinterpret_cast<ShadingStateData*>(context.storage):nullptr;
        return sampleWithState(hit,seed,ss,context.hasXi?context.xi:nullptr);
    }

private:
    BRDFSample sampleWithState(const RayHit ray_hit, unsigned int& seed,
                               ShadingStateData* prepared, const float* xi = nullptr)
    {
        BRDFSample s;

        if (!m_valid) {
            s.scattered = false;
            s.color = Color3(0.0f);
            s.emission = Color3(1.0f, 0.0f, 1.0f); // visible even when the path terminates
            s.wi = Float3(0.0f);
            return s;
        }

        // ------------------------------------------------------------
        // FIX 1a: RayHit already delivers normals flipped toward the
        // ray (exactly what MDL's generated code expects) and records
        // which side was hit in front_face. So the side signal comes
        // straight from the hit — no re-derivation, no re-flipping.
        // ------------------------------------------------------------
        const bool backface = !ray_hit.front_face;

        ShadingStateData local;
        ShadingStateData& ss=prepared?*prepared:local;
        if(!prepared) build_shading_state(ray_hit, ss);

        // NOTE: in single-init mode, execute_init may *modify*
        // ss.base().normal (this is how MaterialX normal maps are
        // applied). Everything after this call sees the perturbed
        // shading normal.
        if (!prepared && m_target.program->target_code->execute_init(
            m_target.program->functions.init_function_index,
            ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0)
            return error_sample();

        // Outgoing direction: from the hit point toward the previous
        // path vertex / camera. Shared by BSDF sampling (k1) and EDF
        // evaluation (FIX 2).
        Float3 d_normalized = ray_hit.d.normalize();
        mi::Float32_3 wo(-d_normalized.x(), -d_normalized.y(), -d_normalized.z());

        // Stochastic coverage: a hole transmits the ray unchanged and emits nothing.
        const float opacity = get_opacity(ss.base());
        if (opacity <= 0.0f || (opacity < 1.0f && rnd(seed) >= opacity)) {
            s.scattered = true;
            s.color = Color3(1.0f);
            s.emission = Color3(0.0f);
            s.wi = ray_hit.d;
            s.delta = true;
            s.null_event = true;
            return s;
        }

        // BSDF sample
        mi::neuraylib::Bsdf_sample_data sd;
        memset(&sd, 0, sizeof(sd));
        sd.k1 = wo;

        // ------------------------------------------------------------
        // FIX 1b: ior1 = medium the incoming ray travels in,
        //         ior2 = medium on the other side of the interface.
        //
        // Frontface hit (ray outside the object):  vacuum -> material
        // Backface hit  (ray inside the object):   material -> vacuum
        //
        // Without this swap, refraction at the *exit* interface of any
        // transmissive material runs Snell's law backwards
        // (1.0 -> ior instead of ior -> 1.0), breaking glass entirely.
        // ------------------------------------------------------------
        const bool thinWalled = get_thin_walled(ss.base());
        if (!backface || thinWalled) {
            sd.ior1 = mi::Float32_3(1.0f, 1.0f, 1.0f);
            sd.ior2 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
        } else {
            sd.ior1 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
            sd.ior2 = mi::Float32_3(1.0f, 1.0f, 1.0f);
        }

        if (xi) {
            sd.xi.x = xi[0]; sd.xi.y = xi[1]; sd.xi.z = xi[2]; sd.xi.w = xi[3];
        } else {
            sd.xi.x = rnd(seed);
            sd.xi.y = rnd(seed);
            sd.xi.z = rnd(seed);
            sd.xi.w = rnd(seed);
        }

        const mi::Size bsdf_index = backface && thinWalled
            ? m_target.program->functions.backface_bsdf_function_index
            : m_target.program->functions.surface_bsdf_function_index;
        // An invalid DF index represents a zero BSDF, not an execution error.
        if (bsdf_index != ~mi::Size(0) &&
            m_target.program->target_code->execute_bsdf_sample(
                bsdf_index + mdl::MaterialFunctions::DF_SAMPLE,
                &sd, ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0)
            return error_sample();

        if (sd.event_type == mi::neuraylib::BSDF_EVENT_ABSORB) {
            s.scattered = false;
            s.color = Color3(0.0f);
            s.wi = Float3(0.0f);
            s.emission = get_emission(ss.base(), wo, backface);
            return s;
        }

        s.scattered = true;
        s.wi = Float3(sd.k2.x, sd.k2.y, sd.k2.z);
        s.color = Color3(sd.bsdf_over_pdf.x, sd.bsdf_over_pdf.y, sd.bsdf_over_pdf.z);
        s.pdf = sd.pdf;
        s.delta = (sd.event_type & mi::neuraylib::BSDF_EVENT_SPECULAR) != 0;
        s.nee_compatible = !s.delta && s.pdf > 0.0f;

        // Crossing the interface enters or leaves the material's volume.
        s.transmitted = (sd.event_type & mi::neuraylib::BSDF_EVENT_TRANSMISSION) != 0;

        s.emission = get_emission(ss.base(), wo, backface);
        return s;
    }

public:

    BRDFEvaluation evaluate(const RayHit& ray_hit, const Float3& wi) override
    { return evaluateWithState(ray_hit,wi,nullptr); }

    BRDFEvaluation evaluatePrepared(const RayHit& hit,const Float3& wi,
        PreparedShadingContext& context) override
    {
        auto* ss=context.owner==this&&context.valid
            ? reinterpret_cast<ShadingStateData*>(context.storage):nullptr;
        return evaluateWithState(hit,wi,ss);
    }

private:
    BRDFEvaluation evaluateWithState(const RayHit& ray_hit,const Float3& wi,
                                     ShadingStateData* prepared)
    {
        BRDFEvaluation result;
        if (!m_valid) return result;
        const bool backface = !ray_hit.front_face;
        ShadingStateData local; ShadingStateData& ss=prepared?*prepared:local;
        if(!prepared) build_shading_state(ray_hit,ss);
        if (!prepared && m_target.program->target_code->execute_init(m_target.program->functions.init_function_index,
            ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return result;
        const bool thinWalled = get_thin_walled(ss.base());
        const mi::Size index = backface && thinWalled
            ? m_target.program->functions.backface_bsdf_function_index
            : m_target.program->functions.surface_bsdf_function_index;
        if (index == ~mi::Size(0)) return result;
        mi::neuraylib::Bsdf_evaluate_data<mi::neuraylib::DF_HSM_NONE> ed;
        memset(&ed, 0, sizeof(ed));
        Float3 wo = -ray_hit.d.normalize(), in = wi.normalize();
        ed.k1 = mi::Float32_3(wo.x(), wo.y(), wo.z());
        ed.k2 = mi::Float32_3(in.x(), in.y(), in.z());
        if (!backface || thinWalled) {
            ed.ior1 = mi::Float32_3(1.0f); ed.ior2 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
        } else {
            ed.ior1 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR); ed.ior2 = mi::Float32_3(1.0f);
        }
        if (m_target.program->target_code->execute_bsdf_evaluate(index + mdl::MaterialFunctions::DF_EVALUATE,
            &ed, ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return result;
        result.value = Color3(ed.bsdf_diffuse.x + ed.bsdf_glossy.x,
                              ed.bsdf_diffuse.y + ed.bsdf_glossy.y,
                              ed.bsdf_diffuse.z + ed.bsdf_glossy.z);
        result.diffuse = Color3(ed.bsdf_diffuse.x, ed.bsdf_diffuse.y, ed.bsdf_diffuse.z);
        result.pdf = ed.pdf;
        result.valid = result.pdf >= 0.0f && std::isfinite(result.pdf);
        return result;
    }
public:
    bool supportsNee(const RayHit&) const override { return m_valid; }

    float cutoutOpacity(const RayHit& ray_hit) override {
        if (!m_valid) return 1.0f;
        if (auto constant = constant_cutout()) return *constant;
        ShadingStateData ss; build_shading_state(ray_hit, ss);
        if (m_target.program->target_code->execute_init(m_target.program->functions.init_function_index,
            ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return 1.0f;
        return get_opacity(ss.base());
    }
    bool provenOpaque() const override {
        const auto constant = constant_cutout();
        return constant && *constant >= 1.0f;
    }

    // The cutout opacity when it is the same everywhere on the material:
    // constant in the class, or depending only on the material's arguments.
    std::optional<ConstantEmission> constantEmission() const override { return m_uniformEmission; }
    bool variesEmission() const override { return m_varyingEmission; }
    bool mayEmitBackFace() const override { return m_varyingEmission && m_target.program->backface_emission; }
    // Light samples evaluate the emitter between the shading point's prepare
    // and its BSDF: that point's texture-runtime context is kept.
    Color3 emittedRadiance(const RayHit& hit) override {
        float origin[3];
        std::copy(std::begin(mdl::NativeTextureRuntime::viewOrigin), std::end(mdl::NativeTextureRuntime::viewOrigin),
                  origin);
        const auto context = mdl::NativeTextureRuntime::primvarContext;
        const Color3 radiance = m_valid ? evaluate_emission(hit) : Color3(0.0f);
        std::copy(origin, origin + 3, mdl::NativeTextureRuntime::viewOrigin);
        mdl::NativeTextureRuntime::primvarContext = context;
        return radiance;
    }

    // geometry.displacement: the offset of a surface point (in the space of
    // the hit, which is the geometry's own space when building it).
    bool hasDisplacement() const override {
        return m_valid && m_target.program && m_target.program->has_displacement &&
            m_target.program->functions.displacement_function_index != ~mi::Size(0);
    }
    Float3 displacement(const RayHit& hit) const override {
        if (!hasDisplacement()) return Float3(0.0f);
        ShadingStateData ss;
        build_shading_state(hit, ss);
        if (m_target.program->target_code->execute_init(m_target.program->functions.init_function_index,
                ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return Float3(0.0f);
        mi::Float32_3 offset(0.0f, 0.0f, 0.0f);
        if (m_target.program->target_code->execute(m_target.program->functions.displacement_function_index,
                ss.base(), m_target.texture_handler(), m_target.argument_data(), &offset) != 0)
            return Float3(0.0f);
        const Float3 result(offset.x, offset.y, offset.z);
        return std::isfinite(result.x()) && std::isfinite(result.y()) && std::isfinite(result.z())
            ? result : Float3(0.0f);
    }

    std::optional<SurfaceMedium> medium(const RayHit& hit, PreparedShadingContext& context) const override
    {
        if (!m_valid) return std::nullopt;
        auto const& functions = m_target.program->functions;
        if (functions.volume_absorption_function_index == ~mi::Size(0) &&
            functions.volume_scattering_function_index == ~mi::Size(0)) return std::nullopt;
        ShadingStateData local;
        ShadingStateData* ss = context.owner == this && context.valid
            ? reinterpret_cast<ShadingStateData*>(context.storage) : nullptr;
        if (!ss) {
            build_shading_state(hit, local);
            if (m_target.program->target_code->execute_init(functions.init_function_index, local.base(),
                    m_target.texture_handler(), m_target.argument_data()) != 0) return std::nullopt;
            ss = &local;
        }
        if (get_thin_walled(ss->base())) return std::nullopt;
        auto color = [&](mi::Size index) {
            mi::Float32_3 value(0.0f, 0.0f, 0.0f);
            if (index == ~mi::Size(0) || m_target.program->target_code->execute(index, ss->base(),
                    m_target.texture_handler(), m_target.argument_data(), &value) != 0) return Color3(0.0f);
            Color3 c(value.x, value.y, value.z);
            for (int i = 0; i < 3; ++i) if (!std::isfinite(c[i]) || c[i] < 0.0f) c[i] = 0.0f;
            return c;
        };
        SurfaceMedium medium;
        medium.absorption = color(functions.volume_absorption_function_index);
        medium.scattering = color(functions.volume_scattering_function_index);
        if (medium.absorption.is_zero() && medium.scattering.is_zero()) return std::nullopt;
        if (functions.volume_anisotropy_function_index != ~mi::Size(0)) {
            mi::Float32 g = 0.0f;
            if (m_target.program->target_code->execute(functions.volume_anisotropy_function_index, ss->base(),
                    m_target.texture_handler(), m_target.argument_data(), &g) == 0 && std::isfinite(g))
                medium.anisotropy = std::clamp(g, -0.99f, 0.99f);
        }
        return medium;
    }

    std::optional<float> constant_cutout() const {
        if (m_target.binding && m_target.binding->constant_cutout_opacity)
            return m_target.binding->constant_cutout_opacity;
        return m_uniformCutout;
    }

    // Required stubs (issue 7 — needed for NEE, not in scope here)
    Float3 sample_direction(const RayHit ray_hit, unsigned int& seed) override {
        return Float3(0.0f);
    }

    Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo,
                          Float3& wi, float u, float v, unsigned int& seed) override {
        return Color3(0.0f);
    }

    Color3 evaluate_emission(const RayHit ray_hit) override {
        if (!m_valid) return Color3(1.0f, 0.0f, 1.0f);

        const bool backface = !ray_hit.front_face;

        ShadingStateData ss;
        build_shading_state(ray_hit, ss);

        if (m_target.program->target_code->execute_init(
            m_target.program->functions.init_function_index,
            ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        // FIX 2: evaluate the EDF toward the actual observer direction
        Float3 d_normalized = ray_hit.d.normalize();
        mi::Float32_3 wo(-d_normalized.x(), -d_normalized.y(), -d_normalized.z());

        // Deterministic evaluation returns expected emission over surface coverage.
        return get_opacity(ss.base()) * get_emission(ss.base(), wo, backface);
    }

private:
    bool get_thin_walled(mi::neuraylib::Shading_state_material& state) const {
        bool value = false;
        const auto index = m_target.program->functions.thin_walled_function_index;
        if (index == ~mi::Size(0)) return false;
        return m_target.program->target_code->execute(index, state, m_target.texture_handler(),
            m_target.argument_data(), &value) == 0 && value;
    }
    static BRDFSample error_sample() {
        BRDFSample s;
        s.scattered = false;
        s.color = Color3(0.0f);
        s.emission = Color3(1.0f, 0.0f, 1.0f);
        s.wi = Float3(0.0f);
        return s;
    }

    float get_opacity(mi::neuraylib::Shading_state_material& state) const {
        if (auto constant = constant_cutout()) return *constant;
        mi::Float32 opacity = 1.0f;
        const mi::Sint32 result = m_target.program->target_code->execute(
            m_target.program->functions.cutout_opacity_function_index,
            state, m_target.texture_handler(), m_target.argument_data(), &opacity);
        // Preserve an opaque surface if evaluation fails instead of losing geometry.
        return result == 0 && std::isfinite(opacity)
            ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    }

    struct ShadingStateData {
        mi::neuraylib::Shading_state_material_with_derivs state;
        mi::neuraylib::tct_deriv_float3 tex_coord;
        mi::Float32_3 tangent_u;
        mi::Float32_3 tangent_v;
        MdlStateMatrixRow identity[4];
        alignas(16) MdlStateFloat4 text_results[128];
        mi::neuraylib::Shading_state_material &base() {
            return reinterpret_cast<mi::neuraylib::Shading_state_material &>(state);
        }
    };
    static_assert(alignof(ShadingStateData) >= 16);

    // Build the MDL shading state.
    //
    // Normals: RayHit already orients both the shading and geometric
    // normal toward the incoming ray, which is the convention MDL's
    // generated code expects — they are used as-is here. The only
    // correction applied is clamping the interpolated shading normal
    // onto the geometric side (smoothed normals can straddle the true
    // surface near silhouettes, which corrupts hemisphere tests).
    //
    // Tangents (FIX 3): hit.tangent is the UV-aligned dP/du supplied by
    // RayHit (Embree vertex attribute slot 2, or analytic for spheres),
    // Gram-Schmidt-orthonormalized against the shading normal, with
    // tangent_v respecting the UV handedness. Falls back to the old
    // normal-derived frame when no tangent is available or it
    // degenerates (poles, broken UVs).
    void build_shading_state(const RayHit& hit, ShadingStateData& ss) const
    {
        memset(&ss, 0, sizeof(ss));
        mdl::NativeTextureRuntime::viewOrigin[0] = hit.o.x();
        mdl::NativeTextureRuntime::viewOrigin[1] = hit.o.y();
        mdl::NativeTextureRuntime::viewOrigin[2] = hit.o.z();
        mdl::NativeTextureRuntime::primvarContext = {hit.primvarMesh, unsigned(hit.primid), hit.bary_u, hit.bary_v,
            [](const void* mesh, const char* name, unsigned primitive, float u, float v, float* out, int count) {
                return static_cast<Mesh const*>(mesh)->interpolatePrimvar(name, primitive, u, v, out, count);
            }, hit.instancePrimvars,
            [](const void* instance, const char* name, float* out, int count) {
                return static_cast<InstancePrimvars const*>(instance)->lookup(name, out, count);
            }};

        // UV
        ss.tex_coord.val = mi::Float32_3(hit.u, hit.v, 0.0f);
        const bool filtered = hit.texture_footprint.filtering ==
            prism::TextureFiltering::Filtered;
        ss.tex_coord.dx = filtered ? mi::Float32_3(
            hit.texture_footprint.dudx, hit.texture_footprint.dvdx, 0.0f)
            : mi::Float32_3(0.0f);
        ss.tex_coord.dy = filtered ? mi::Float32_3(
            hit.texture_footprint.dudy, hit.texture_footprint.dvdy, 0.0f)
            : mi::Float32_3(0.0f);

        // Ray-facing normals straight from the hit. n_g is populated by
        // RayHit before shading-normal interpolation; the is_zero()
        // guard only covers hand-built RayHits that never set it.
        Float3 n_g = hit.n_g.is_zero() ? hit.n : hit.n_g;
        Float3 n = hit.n;
        if (dot(n, n_g) < 0.0f)
            n = -n;

        // --- Tangent frame (FIX 3) ---
        Float3 t;
        bool have_uv_tangent = !hit.tangent.is_zero();

        if (have_uv_tangent) {
            // Gram-Schmidt: project dP/du into the plane perpendicular to n
            Float3 t_proj = hit.tangent - dot(n, hit.tangent) * n;

            // Degenerate after projection (tangent parallel to normal,
            // e.g. sphere poles or broken UVs) -> fall back below.
            if (dot(t_proj, t_proj) < 1e-12f)
                have_uv_tangent = false;
            else
                t = t_proj.normalize();
        }

        if (!have_uv_tangent) {
            // Fallback: arbitrary frame from the normal (previous
            // behavior). Fine for UV-less geometry; wrong for
            // anisotropy/normal maps, which is why geometry should
            // supply hit.tangent.
            if (std::abs(n.x()) > std::abs(n.z()))
                t = Float3(-n.y(), n.x(), 0.0f).normalize();
            else
                t = Float3(0.0f, -n.z(), n.y()).normalize();
        }

        Float3 b = cross(n, t);

        // Respect UV handedness (mirrored UV islands / glTF TANGENT.w /
        // the sphere parameterization, which is left-handed here).
        if (hit.bitangent_sign < 0.0f)
            b = -b;

        ss.tangent_u = mi::Float32_3(t.x(), t.y(), t.z());
        ss.tangent_v = mi::Float32_3(b.x(), b.y(), b.z());

        // Identity transforms
        ss.identity[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
        ss.identity[1] = { 0.0f, 1.0f, 0.0f, 0.0f };
        ss.identity[2] = { 0.0f, 0.0f, 1.0f, 0.0f };
        ss.identity[3] = { 0.0f, 0.0f, 0.0f, 1.0f };

        // MDL state
        ss.state.position.val = mi::Float32_3(hit.p.x(), hit.p.y(), hit.p.z());
        ss.state.position.dx = mi::Float32_3(hit.dpdx.x(),hit.dpdx.y(),hit.dpdx.z());
        ss.state.position.dy = mi::Float32_3(hit.dpdy.x(),hit.dpdy.y(),hit.dpdy.z());
        ss.state.normal      = mi::Float32_3(n.x(), n.y(), n.z());
        ss.state.geom_normal = mi::Float32_3(n_g.x(), n_g.y(), n_g.z());
        ss.state.tangent_u       = &ss.tangent_u;
        ss.state.tangent_v       = &ss.tangent_v;
        ss.state.text_coords     = &ss.tex_coord;
        ss.state.animation_time  = 0.0f;
        ss.state.text_results    = ss.text_results;
        ss.state.ro_data_segment = m_target.program ? m_target.program->ro_data_segment : nullptr;
        ss.state.world_to_object = ss.identity;
        ss.state.object_to_world = ss.identity;
        ss.state.object_id       = 0;
        ss.state.meters_per_scene_unit = 1.0f;
    }

    // FIX 2: EDF k1 is the direction toward the observer (-ray.d), not
    // the surface normal. With the normal, every EDF was evaluated at
    // exactly normal incidence: hemisphere tests always passed and
    // directional EDFs (spot_edf, measured) were flat-out wrong.
    //
    // Backface gating: MDL defines surface.emission for the front side
    // only; only thin-walled materials emit from the back (via
    // backface.*). Since RayHit flips normals toward
    // the ray, evaluating the surface EDF on a backface hit would make
    // the inside of an emissive shell glow — return black instead.
    Color3 get_emission(mi::neuraylib::Shading_state_material& state,
                        const mi::Float32_3& wo,
                        bool backface) const
    {
        if (backface && !get_thin_walled(state))
            return Color3(0.0f);

        mi::Float32_3 ei(0.0f, 0.0f, 0.0f);
        const mi::Size intensity_index = backface
            ? m_target.program->functions.backface_emission_intensity_function_index
            : m_target.program->functions.surface_emission_intensity_function_index;
        if (m_target.program->target_code->execute(
            intensity_index,
            state, m_target.texture_handler(), m_target.argument_data(), &ei) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        if (ei.x <= 0.0f && ei.y <= 0.0f && ei.z <= 0.0f)
            return Color3(0.0f);

        mi::neuraylib::Edf_evaluate_data<mi::neuraylib::DF_HSM_NONE> ed;
        memset(&ed, 0, sizeof(ed));
        ed.k1 = wo;

        const mi::Size edf_index = backface
            ? m_target.program->functions.backface_edf_function_index
            : m_target.program->functions.surface_edf_function_index;
        if (edf_index == ~mi::Size(0)) return Color3(0.0f);
        if (m_target.program->target_code->execute_edf_evaluate(
            edf_index + mdl::MaterialFunctions::DF_EVALUATE,
            &ed, state, m_target.texture_handler(), m_target.argument_data()) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        return Color3(ei.x * ed.edf.x, ei.y * ed.edf.y, ei.z * ed.edf.z);
    }

    // A cutout depending only on the arguments is evaluated once, at an
    // arbitrary shading point.
    void evaluateUniformCutout() {
        if (!m_valid || !m_target.program || !m_target.program->uniform_cutout ||
            (m_target.binding && m_target.binding->constant_cutout_opacity)) return;
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit hit(miss);
        hit.n = hit.n_g = Float3(0.0f, 0.0f, 1.0f);
        hit.tangent = Float3(1.0f, 0.0f, 0.0f);
        hit.u = hit.v = 0.0f;
        hit.front_face = true;
        ShadingStateData ss;
        build_shading_state(hit, ss);
        if (m_target.program->target_code->execute_init(m_target.program->functions.init_function_index,
                ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return;
        mi::Float32 opacity = 1.0f;
        if (m_target.program->target_code->execute(m_target.program->functions.cutout_opacity_function_index,
                ss.base(), m_target.texture_handler(), m_target.argument_data(), &opacity) == 0 &&
            std::isfinite(opacity))
            m_uniformCutout = std::clamp(opacity, 0.0f, 1.0f);
    }

    // Whether the BSDF transmits: a few samples from each side at two angles
    // of incidence, at the centre of the texture space, see a transmission
    // event (refraction, thin-walled translucency). Cutout holes do not count.
    void evaluateTransmission() {
        m_transmissive = false;
        if (!m_valid || !m_target.program) return;
        for (const bool front : {true, false}) {
            for (const float cosine : {1.0f, 0.5f}) {
                RTCRayHit miss{};
                miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                RayHit hit(miss);
                hit.n = hit.n_g = Float3(0.0f, 0.0f, 1.0f);
                hit.tangent = Float3(1.0f, 0.0f, 0.0f);
                hit.u = hit.v = 0.5f;
                hit.front_face = front;
                const float sine = std::sqrt(1.0f - cosine * cosine);
                hit.d = Float3(sine, 0.0f, -cosine);
                hit.o = hit.p - hit.d;
                for (int i = 0; i < 16; ++i) {
                    const float xi[4] = {(float(i % 4) + 0.5f) / 4.0f, (float(i / 4) + 0.5f) / 4.0f,
                                         (float(i) + 0.5f) / 16.0f, (float(15 - i) + 0.5f) / 16.0f};
                    unsigned seed = 1u + unsigned(i);
                    const BRDFSample s = sampleWithState(hit, seed, nullptr, xi);
                    if (s.scattered && s.transmitted && !s.null_event) {
                        m_transmissive = true;
                        return;
                    }
                }
            }
        }
    }

    // Uniform emission is evaluated once, toward the normal of the front face.
    // Other emission that is not provably zero varies (m_varyingEmission):
    // textures, a varying cutout, or a directional EDF.
    void evaluateUniformEmission() {
        m_uniformEmission.reset();
        m_varyingEmission = false;
        if (!m_valid || !m_target.program) return;
        auto const& program = *m_target.program;
        const bool surfaceEmits = !program.emission_zero &&
            program.functions.surface_edf_function_index != ~mi::Size(0);
        const bool backEmits = program.backface_emission &&
            program.functions.backface_edf_function_index != ~mi::Size(0);
        if (!surfaceEmits && !backEmits) return;
        m_varyingEmission = true;
        if (!surfaceEmits || !program.uniform_emission) return;
        // A varying cutout scales the emission per point, but cannot make
        // zero emission positive: that is evaluated without it.
        const auto cutout = constant_cutout();
        if (constantEmissionValue(cutout.value_or(1.0f))) {
            if (cutout) m_varyingEmission = false;
            else m_uniformEmission.reset();
            return;
        }
        // Uniform front emission that is zero or directional; a back face
        // that may emit still varies, unless it is the front's own EDF.
        m_varyingEmission = (backEmits && !program.backface_emission_matches_surface) ||
                            m_directionalEmission;
    }

    // The constant front-face emission of a uniform EDF; false if it is zero,
    // not finite, or directional (m_directionalEmission).
    bool constantEmissionValue(float cutout) {
        m_directionalEmission = false;
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit hit(miss);
        hit.n = hit.n_g = Float3(0.0f, 0.0f, 1.0f);
        hit.tangent = Float3(1.0f, 0.0f, 0.0f);
        hit.u = hit.v = 0.0f;
        hit.front_face = true;
        ShadingStateData ss;
        build_shading_state(hit, ss);
        if (m_target.program->target_code->execute_init(m_target.program->functions.init_function_index,
                ss.base(), m_target.texture_handler(), m_target.argument_data()) != 0) return false;
        auto finite = [](Color3 const& c) {
            return std::isfinite(c.x()) && std::isfinite(c.y()) && std::isfinite(c.z());
        };
        auto positive = [](Color3 const& c) { return c.x() > 0.0f || c.y() > 0.0f || c.z() > 0.0f; };
        const Color3 radiance = get_emission(ss.base(), mi::Float32_3(0.0f, 0.0f, 1.0f), false) * cutout;
        if (!finite(radiance)) return false;
        // A df::directional_factor (OpenPBR and standard_surface layer their
        // emission under the coat this way) is isotropic when its tints agree,
        // as when these arguments disable the coat: class compilation keeps
        // the factor that folding the arguments would remove.
        if (m_target.program->directional_emission) {
            for (const float angle : {0.5f, 1.0f, 1.3f, 1.5f}) {
                const mi::Float32_3 wo(std::sin(angle), 0.0f, std::cos(angle));
                const Color3 other = get_emission(ss.base(), wo, false) * cutout;
                for (int c = 0; c < 3; ++c)
                    if (!(std::abs(other[c] - radiance[c]) <= 1.0e-4f * std::max(radiance[c], 1.0e-6f))) {
                        m_directionalEmission = true;
                        return false;
                    }
            }
        }
        if (!positive(radiance)) return false;
        m_uniformEmission = ConstantEmission{radiance, false};
        return true;
    }

    std::optional<float> m_uniformCutout;
    std::optional<ConstantEmission> m_uniformEmission;
    bool m_varyingEmission = false;
    bool m_directionalEmission = false;
    bool m_transmissive = false;

    // MDL pipeline objects — kept alive for the material's lifetime
    std::unique_ptr<mdl::MaterialCompiler> m_compiler;
    mdl::CompiledMaterial m_compiled;
    mdl::TargetCode m_target;
    std::vector<GeneratedUniform> m_generated_uniforms;
    bool m_foldedParameters = false;
    bool m_valid = false;
};
