#pragma once

#include "material.h"
#include "mdl/context.h"
#include "mdl/material_compiler.h"
#include "mdl/backend_compiler.h"
#include "materialx/loader.h"
#include "materialx/mdl_generator.h"

#include <mi/mdl_sdk.h>
#include <memory>
#include <string>
#include <iostream>
#include <algorithm>
#include <cmath>

// Self-contained MDL material that takes a .mtlx file path, compiles
// it through the full MaterialX -> MDL -> native CPU pipeline, and
// provides PrismRT's Material interface for path tracing.
class MDLMaterial : public Material {
public:
    // Compiles a .mtlx file into a callable MDL material.
    //   mtlx_path:         path to the .mtlx file
    //   mdl_context:       initialized MDL SDK context (from main)
    //   mx_libraries_path: path to MaterialX install libraries/
    //   mx_mdl_path:       path to MaterialX install libraries/mdl/
    //   material_name:     name of the material node (empty = use first material found)
    MDLMaterial(const std::string& mtlx_path,
                mdl::Context& mdl_context,
                const std::string& mx_libraries_path,
                const std::string& mx_mdl_path,
                const std::string& material_name = "")
    {
        std::cout << "[MDLMaterial] Compiling: " << mtlx_path << "\n";

        // Phase 3: MaterialX -> MDL code generation
        MDLGenerator generator(mx_libraries_path);
        DocumentLoader doc_loader(generator.get_stdlib(), true);

        MaterialX::DocumentPtr doc = doc_loader.load(mtlx_path);
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
            std::cout << "[MDLMaterial] Auto-detected material: " << mat_name << "\n";
        }

        GeneratedMDL gen = generator.generate(doc, mat_name);
        if (!gen.success) {
            std::cerr << "[MDLMaterial] ERROR: MDL generation failed: " << gen.error_message << "\n";
            m_valid = false;
            return;
        }

        // Phase 4: Load MDL into SDK and compile
        m_compiler = std::make_unique<mdl::MaterialCompiler>(mdl_context, mx_mdl_path);
        m_compiled = m_compiler->compile(gen, mtlx_path);

        if (!m_compiled.success) {
            std::cerr << "[MDLMaterial] ERROR: Compilation failed: " << m_compiled.error_message << "\n";
            m_valid = false;
            return;
        }

        // Phase 5: Backend compilation to native CPU code
        mdl::BackendCompiler backend(mdl_context);
        m_target = backend.generate_target_code(m_compiled);

        if (!m_target.success) {
            std::cerr << "[MDLMaterial] ERROR: Backend compilation failed: " << m_target.error_message << "\n";
            m_compiled.release();
            m_valid = false;
            return;
        }

        m_valid = true;
        // NOTE: prints the *resolved* name (material_name may be empty)
        std::cout << "[MDLMaterial] OK: " << m_target.target_code->get_callable_function_count()
                  << " functions compiled from '" << mat_name << "'\n";
    }

    ~MDLMaterial() {
        m_compiled.release();
    }

    bool is_valid() const { return m_valid; }

    // Accessor for the target code (needed by MaterialXTest scene)
    const mdl::TargetCode& get_target_code() const { return m_target; }

    // ----------------------------------------------------------------
    // PrismRT Material interface
    // ----------------------------------------------------------------

    BRDFSample sample(const RayHit ray_hit, unsigned int& seed) override
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

        ShadingStateData ss;
        build_shading_state(ray_hit, ss);

        // NOTE: in single-init mode, execute_init may *modify*
        // ss.state.normal (this is how MaterialX normal maps are
        // applied). Everything after this call sees the perturbed
        // shading normal.
        if (m_target.target_code->execute_init(
            m_target.functions.init_function_index,
            ss.state, nullptr, nullptr) != 0)
            return error_sample();

        // Outgoing direction: from the hit point toward the previous
        // path vertex / camera. Shared by BSDF sampling (k1) and EDF
        // evaluation (FIX 2).
        Float3 d_normalized = ray_hit.d.normalize();
        mi::Float32_3 wo(-d_normalized.x(), -d_normalized.y(), -d_normalized.z());

        // Stochastic coverage: a hole transmits the ray unchanged and emits nothing.
        const float opacity = get_opacity(ss.state);
        if (opacity <= 0.0f || (opacity < 1.0f && rnd(seed) >= opacity)) {
            s.scattered = true;
            s.color = Color3(1.0f);
            s.emission = Color3(0.0f);
            s.wi = ray_hit.d;
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
        if (!backface || m_target.is_thin_walled) {
            sd.ior1 = mi::Float32_3(1.0f, 1.0f, 1.0f);
            sd.ior2 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
        } else {
            sd.ior1 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
            sd.ior2 = mi::Float32_3(1.0f, 1.0f, 1.0f);
        }

        sd.xi.x = rnd(seed);
        sd.xi.y = rnd(seed);
        sd.xi.z = rnd(seed);
        sd.xi.w = rnd(seed);

        const mi::Size bsdf_index = backface && m_target.is_thin_walled
            ? m_target.functions.backface_bsdf_function_index
            : m_target.functions.surface_bsdf_function_index;
        // An invalid DF index represents a zero BSDF, not an execution error.
        if (bsdf_index != ~mi::Size(0) &&
            m_target.target_code->execute_bsdf_sample(
                bsdf_index + mdl::MaterialFunctions::DF_SAMPLE,
                &sd, ss.state, nullptr, nullptr) != 0)
            return error_sample();

        if (sd.event_type == mi::neuraylib::BSDF_EVENT_ABSORB) {
            s.scattered = false;
            s.color = Color3(0.0f);
            s.wi = Float3(0.0f);
            s.emission = get_emission(ss.state, wo, backface);
            return s;
        }

        s.scattered = true;
        s.wi = Float3(sd.k2.x, sd.k2.y, sd.k2.z);
        s.color = Color3(sd.bsdf_over_pdf.x, sd.bsdf_over_pdf.y, sd.bsdf_over_pdf.z);

        // Hook for later (volume absorption, issue 6): the sampled event
        // tells us whether the ray crossed the interface.
        // bool transmitted = (sd.event_type & mi::neuraylib::BSDF_EVENT_TRANSMISSION) != 0;

        s.emission = get_emission(ss.state, wo, backface);
        return s;
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

        if (m_target.target_code->execute_init(
            m_target.functions.init_function_index,
            ss.state, nullptr, nullptr) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        // FIX 2: evaluate the EDF toward the actual observer direction
        Float3 d_normalized = ray_hit.d.normalize();
        mi::Float32_3 wo(-d_normalized.x(), -d_normalized.y(), -d_normalized.z());

        // Deterministic evaluation returns expected emission over surface coverage.
        return get_opacity(ss.state) * get_emission(ss.state, wo, backface);
    }

private:
    static BRDFSample error_sample() {
        BRDFSample s;
        s.scattered = false;
        s.color = Color3(0.0f);
        s.emission = Color3(1.0f, 0.0f, 1.0f);
        s.wi = Float3(0.0f);
        return s;
    }

    float get_opacity(mi::neuraylib::Shading_state_material& state) const {
        mi::Float32 opacity = 1.0f;
        const mi::Sint32 result = m_target.target_code->execute(
            m_target.functions.cutout_opacity_function_index,
            state, nullptr, nullptr, &opacity);
        // Preserve an opaque surface if evaluation fails instead of losing geometry.
        return result == 0 && std::isfinite(opacity)
            ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    }

    struct ShadingStateData {
        mi::neuraylib::Shading_state_material state;
        mi::Float32_3 tex_coord;
        mi::Float32_3 tangent_u;
        mi::Float32_3 tangent_v;
        mi::neuraylib::tct_float4 identity[4];
        mi::neuraylib::tct_float4 text_results[128];
    };

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

        // UV
        ss.tex_coord = mi::Float32_3(hit.u, hit.v, 0.0f);

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
        ss.state.position    = mi::Float32_3(hit.p.x(), hit.p.y(), hit.p.z());
        ss.state.normal      = mi::Float32_3(n.x(), n.y(), n.z());
        ss.state.geom_normal = mi::Float32_3(n_g.x(), n_g.y(), n_g.z());
        ss.state.tangent_u       = &ss.tangent_u;
        ss.state.tangent_v       = &ss.tangent_v;
        ss.state.text_coords     = &ss.tex_coord;
        ss.state.animation_time  = 0.0f;
        ss.state.text_results    = ss.text_results;
        ss.state.ro_data_segment = m_target.ro_data_segment;
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
        if (backface && !m_target.is_thin_walled)
            return Color3(0.0f);

        mi::Float32_3 ei(0.0f, 0.0f, 0.0f);
        const mi::Size intensity_index = backface
            ? m_target.functions.backface_emission_intensity_function_index
            : m_target.functions.surface_emission_intensity_function_index;
        if (m_target.target_code->execute(
            intensity_index,
            state, nullptr, nullptr, &ei) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        if (ei.x <= 0.0f && ei.y <= 0.0f && ei.z <= 0.0f)
            return Color3(0.0f);

        mi::neuraylib::Edf_evaluate_data<mi::neuraylib::DF_HSM_NONE> ed;
        memset(&ed, 0, sizeof(ed));
        ed.k1 = wo;

        const mi::Size edf_index = backface
            ? m_target.functions.backface_edf_function_index
            : m_target.functions.surface_edf_function_index;
        if (edf_index == ~mi::Size(0)) return Color3(0.0f);
        if (m_target.target_code->execute_edf_evaluate(
            edf_index + mdl::MaterialFunctions::DF_EVALUATE,
            &ed, state, nullptr, nullptr) != 0)
            return Color3(1.0f, 0.0f, 1.0f);

        return Color3(ei.x * ed.edf.x, ei.y * ed.edf.y, ei.z * ed.edf.z);
    }

    // MDL pipeline objects — kept alive for the material's lifetime
    std::unique_ptr<mdl::MaterialCompiler> m_compiler;
    mdl::CompiledMaterial m_compiled;
    mdl::TargetCode m_target;
    bool m_valid = false;
};
