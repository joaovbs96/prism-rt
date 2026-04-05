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

// Self-contained MDL material that takes a .mtlx file path, compiles
// it through the full MaterialX -> MDL -> native CPU pipeline, and
// provides PrismRT's Material interface for path tracing.
class MDLMaterial : public Material {
public:
    // Compiles a .mtlx file into a callable MDL material.
    //   mtlx_path:         path to the .mtlx file
    //   material_name:     name of the material node inside the .mtlx
    //   mdl_context:       initialized MDL SDK context (from main)
    //   mx_libraries_path: path to MaterialX install libraries/
    //   mx_mdl_path:       path to MaterialX install libraries/mdl/
    MDLMaterial(const std::string& mtlx_path,
                const std::string& material_name,
                mdl::Context& mdl_context,
                const std::string& mx_libraries_path,
                const std::string& mx_mdl_path)
    {
        std::cout << "[MDLMaterial] Compiling: " << mtlx_path << "\n";

        // Phase 3: MaterialX -> MDL code generation
        MDLGenerator generator(mx_libraries_path);
        DocumentLoader doc_loader(generator.get_stdlib());

        MaterialX::DocumentPtr doc = doc_loader.load(mtlx_path);
        if (!doc) {
            std::cerr << "[MDLMaterial] ERROR: Failed to load " << mtlx_path << "\n";
            m_valid = false;
            return;
        }

        GeneratedMDL gen = generator.generate(doc, material_name);
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
        std::cout << "[MDLMaterial] OK: " << m_target.target_code->get_callable_function_count()
                  << " functions compiled from '" << material_name << "'\n";
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
            s.color = Color3(1.0f, 0.0f, 1.0f); // magenta = error
            s.emission = Color3(0.0f);
            s.wi = Float3(0.0f);
            return s;
        }

        ShadingStateData ss = build_shading_state(ray_hit);

        m_target.target_code->execute_init(
            m_target.functions.init_function_index,
            ss.state, nullptr, nullptr);

        // Cutout opacity
        float opacity = 1.0f;
        {
            mi::Float32 opacity_result = 1.0f;
            m_target.target_code->execute(
                m_target.functions.cutout_opacity_function_index,
                ss.state, nullptr, nullptr, &opacity_result);
            opacity = opacity_result;
        }
        if (opacity < rnd(seed)) {
            s.scattered = true;
            s.color = Color3(1.0f);
            s.emission = Color3(0.0f);
            s.wi = ray_hit.d;
            return s;
        }

        // BSDF sample
        mi::neuraylib::Bsdf_sample_data sd;
        memset(&sd, 0, sizeof(sd));
        sd.k1 = mi::Float32_3(-ray_hit.d.x(), -ray_hit.d.y(), -ray_hit.d.z());
        sd.ior1 = mi::Float32_3(1.0f, 1.0f, 1.0f);
        sd.ior2 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
        sd.xi.x = rnd(seed);
        sd.xi.y = rnd(seed);
        sd.xi.z = rnd(seed);
        sd.xi.w = rnd(seed);

        m_target.target_code->execute_bsdf_sample(
            m_target.functions.surface_bsdf_function_index + mdl::MaterialFunctions::DF_SAMPLE,
            &sd, ss.state, nullptr, nullptr);

        if (sd.event_type == mi::neuraylib::BSDF_EVENT_ABSORB) {
            s.scattered = false;
            s.color = Color3(0.0f);
            s.wi = Float3(0.0f);
            s.emission = get_emission(ss.state);
            return s;
        }

        s.scattered = true;
        s.wi = Float3(sd.k2.x, sd.k2.y, sd.k2.z);
        s.color = Color3(sd.bsdf_over_pdf.x, sd.bsdf_over_pdf.y, sd.bsdf_over_pdf.z);
        s.emission = get_emission(ss.state);
        return s;
    }

    // Required stubs
    Float3 sample_direction(const RayHit ray_hit, unsigned int& seed) override {
        return Float3(0.0f);
    }

    Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo,
                          Float3& wi, float u, float v, unsigned int& seed) override {
        return Color3(0.0f);
    }

    Color3 evaluate_emission(const RayHit ray_hit) override {
        if (!m_valid) return Color3(0.0f);
        ShadingStateData ss = build_shading_state(ray_hit);
        m_target.target_code->execute_init(
            m_target.functions.init_function_index,
            ss.state, nullptr, nullptr);
        return get_emission(ss.state);
    }

private:
    struct ShadingStateData {
        mi::neuraylib::Shading_state_material state;
        mi::Float32_3 tex_coord;
        mi::Float32_3 tangent_u;
        mi::Float32_3 tangent_v;
        mi::neuraylib::tct_float4 identity[4];
        mi::Float32_4 text_results[16];
    };

    ShadingStateData build_shading_state(const RayHit& hit) const
    {
        ShadingStateData ss;
        memset(&ss, 0, sizeof(ss));

        // UV
        ss.tex_coord = mi::Float32_3(hit.u, hit.v, 0.0f);

        // Build tangent frame from shading normal
        Float3 n = hit.n;
        Float3 t;
        if (std::abs(n.x()) > std::abs(n.z()))
            t = Float3(-n.y(), n.x(), 0.0f).normalize();
        else
            t = Float3(0.0f, -n.z(), n.y()).normalize();
        Float3 b = cross(n, t);

        ss.tangent_u = mi::Float32_3(t.x(), t.y(), t.z());
        ss.tangent_v = mi::Float32_3(b.x(), b.y(), b.z());

        // Identity transforms
        ss.identity[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
        ss.identity[1] = { 0.0f, 1.0f, 0.0f, 0.0f };
        ss.identity[2] = { 0.0f, 0.0f, 1.0f, 0.0f };
        ss.identity[3] = { 0.0f, 0.0f, 0.0f, 1.0f };

        // MDL state
        ss.state.position    = mi::Float32_3(hit.p.x(), hit.p.y(), hit.p.z());
        ss.state.normal      = mi::Float32_3(hit.n.x(), hit.n.y(), hit.n.z());
        ss.state.geom_normal = hit.n_g.is_zero()
            ? mi::Float32_3(hit.n.x(), hit.n.y(), hit.n.z())
            : mi::Float32_3(hit.n_g.x(), hit.n_g.y(), hit.n_g.z());
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

        return ss;
    }

    Color3 get_emission(mi::neuraylib::Shading_state_material& state) const
    {
        mi::Float32_3 ei(0.0f, 0.0f, 0.0f);
        m_target.target_code->execute(
            m_target.functions.surface_emission_intensity_function_index,
            state, nullptr, nullptr, &ei);

        if (ei.x <= 0.0f && ei.y <= 0.0f && ei.z <= 0.0f)
            return Color3(0.0f);

        mi::neuraylib::Edf_evaluate_data<mi::neuraylib::DF_HSM_NONE> ed;
        memset(&ed, 0, sizeof(ed));
        ed.k1 = state.normal;

        m_target.target_code->execute_edf_evaluate(
            m_target.functions.surface_edf_function_index + mdl::MaterialFunctions::DF_EVALUATE,
            &ed, state, nullptr, nullptr);

        return Color3(ei.x * ed.edf.x, ei.y * ed.edf.y, ei.z * ed.edf.z);
    }

    // MDL pipeline objects — kept alive for the material's lifetime
    std::unique_ptr<mdl::MaterialCompiler> m_compiler;
    mdl::CompiledMaterial m_compiled;
    mdl::TargetCode m_target;
    bool m_valid = false;
};