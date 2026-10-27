# PrismRT SDK and Python API: design

Status: design only (no code yet). Plan context: `docs/PLAN.md`, "Then: renderer
SDK and Python API". Line numbers refer to commit `c0e44c1`.

Contents:
1. Goals and non-goals
2. Global-state inventory
3. Public API sketch (C++)
4. Header split and module boundaries
5. Motion reservation
6. Python API sketch (nanobind)
7. Migration plan
8. Open questions for João

## 1. Goals and non-goals

### Goals
- One renderer library that the CLI, the Hydra delegate and Python all use.
  Today each front end drives internals directly (`SceneManager`,
  `RenderDevice`, `MaterialService`, process-wide setters).
- A context object (`prism::Renderer`) that owns what is process-wide today:
  colour management, MaterialX/MDL material system, texture service, caches,
  SSS fallback, binding purposes, progress and logging. Each renderer can have
  its own settings; two renderers in one process do not leak into each other.
- Public headers with only std and PrismRT types: no Embree, MDL SDK,
  MaterialX, TBB, OIIO, OCIO or USD types.
- Scene building through handles (meshes, spheres, instances, materials,
  lights, environment, cameras) with incremental edits and a commit point.
  Hydra's edit path must fit on it.
- Rendering through sessions: devices (CPU, Vulkan, Metal), settings, batch
  and progressive rendering, AOVs, denoising, cancellation, progress.
- Loaders as optional modules: `prism_usd` (USD stage + UsdShade + render
  settings/products), `prism_gltf`, `prism_obj`. The core links without USD.
- A thin Python layer (nanobind) over the C++ API: NumPy/DLPack zero-copy,
  GIL released while rendering, cancellation through the progress callback,
  `pxr` stages shared through `UsdUtils.StageCache`.
- Motion blur shapes reserved in the API now (section 5), implemented later
  (Phase 8b).
- GPU rendering from Python: Vulkan (Windows/Linux), Metal (macOS).

### Non-goals (for the first SDK release)
- No ABI promise and no C API until a first tagged release (open question 8.2).
- No new rendering features (motion blur, curves, volumes come later).
- No rewrite of the integrator, the devices or the material pipeline: they
  move behind the API, mostly unchanged.
- Python packaging (wheels, bundled DLLs, load order) is its own task; this
  doc lists only the constraints it must meet (6.7).
- Not a scene-graph: no hierarchy, no named prims in the core. Front ends keep
  their own path tables (USD paths, Hydra picking) and map them to handles and
  user ids.

### Inspiration (short)
- Embree: opaque handles, explicit `rtcCommitScene`, edits are not seen until
  commit, single writer per object, error callback per device.
- OIDN: C API plus a header-only C++ wrapper (`oidn::DeviceRef`,
  `FilterRef`), `commit()` before execute, errors through `getError()` or a
  callback, device type chosen at creation (CPU, CUDA, SYCL, Metal, HIP).
- OSPRay: objects with parameters + `ospCommit`; `ospRenderFrame` returns an
  `OSPFuture` with `ospWait`, `ospCancel`, `ospGetProgress`.
- Cycles standalone: `Session` + `SessionParams` + `Scene`; `Session::start`,
  `wait`, `cancel`; a `Progress` object with update callbacks; scene edits
  under `scene->mutex` and `tag_update` flags.
- Mitsuba 3: Python-first; `mi.set_variant` picks the backend,
  `mi.load_file`/`mi.load_dict` build scenes, `mi.render(scene, spp=...)`
  returns a tensor that converts zero-copy to NumPy/DLPack.

## 2. Global-state inventory

How to read it:
- "Today" is who creates and owns the state now.
- Target:
  - **Runtime**: an internal, process-wide, ref-counted object behind every
    `Renderer` (not public). For state that must be unique per process (the
    MDL SDK) or is a content-addressed cache that is safe to share.
  - **Renderer**: per context (public `prism::Renderer`).
  - **Session**: per render session (device + film + settings).
  - **Scene**: per scene.
  - **Front end**: moves out of the core into the CLI, Hydra or Python.
  - **Stays global**: with the reason.
- Found with: `grep` for `static`, `thread_local`, `call_once`, `*new`,
  `Shared()`, `shared(`, `Acquire()`, `instance()`, `Current()`, `Active()`,
  `getenv`, and namespace-scope variables (`g*`, anonymous namespaces) over
  `src/` and `shaders/`.
- Total: **66 rows** (some rows group several sites of one kind, e.g. one row
  per env-var family). 89 distinct environment variables are read (85 `PRISM_*`
  plus `OCIO`, `MDL_SDK_ROOT`, `LOCALAPPDATA`, `HOME`; rows 2, 8,
  11–22, 58–60, 63).

### 2.1 Process runtime (MDL SDK, Embree, oneTBB, OIDN)

| # | State | Where | Today | Target |
|---|---|---|---|---|
| 1 | MDL SDK context (`INeuray`, database, factories) | `SharedMdlContext()` `src/materialx/material_service.cpp:767-777`; `gSharedMdlContext` `:162` | Leaked process-lifetime singleton; CLI `standalone_cli.cpp:908`, Hydra `scene_bridge.cpp:4641,6207` | **Stays global** (Runtime). The MDL SDK runs one `INeuray` per process and compiled target code holds SDK handles across renderer lifetimes. Renderer keeps a reference. |
| 2 | MDL SDK library loader + `MDL_SDK_ROOT` | `load_mdl_sdk` `static Loader` `src/mdl/loader.cpp:278`; env `src/mdl/loader.cpp:14` | Static | **Stays global** (Runtime): keeps `libmdl_sdk` loaded. The root becomes `RuntimeOptions::mdlSdkRoot`, first renderer wins (warn on a different value). |
| 3 | MDL shutdown hooks | `ShutdownHooks()` `src/mdl/context.cpp:54-58` | Static vector | **Stays global** (Runtime): caches holding SDK handles register here. |
| 4 | Embree device | CLI `EmbreeDevice` `src/standalone_cli.cpp:742-745`; Hydra `rtcNewDevice` per bridge `src/hydra/scene_bridge.cpp:4613` | One per CLI run / per `SceneBridge::Impl` | **Renderer** (one `RTCDevice`, memory monitor per renderer). |
| 5 | OIDN CPU denoiser | `SharedCpuDenoiser` `src/device/denoiser.cpp:143-158` | Static, lazily created | **Runtime** (OIDN CPU device uses the process TBB); exposed through the Renderer. |
| 6 | oneTBB scheduler | implicit global arena for every `tbb::parallel_for` | Global | **Renderer**: a `tbb::task_arena` with `RendererOptions::threads` (0 = all). OIDN keeps the global arena. |

### 2.2 Configuration held in globals

| # | State | Where | Today | Target |
|---|---|---|---|---|
| 7 | Active colour management | `gActive` `src/color/color_management.cpp:273-300` (`Active`, `SetActive`, `Activate`). Set: `standalone_cli.cpp:862`, `scene_bridge.cpp:5219,5376`. Read at load: `lights.h:176,179,196`, `environment.h:245`, `texture.h:62,122,242`, `texture_service.cpp:654`, `mdl_material.h:98`, `mdl_generator.cpp:272`, `prepared_template.h:402,438`, `gltf_mesh.h:135,433`, `scene_bridge.cpp:2486`. Read **while rendering**: `cpu_device.cpp:31`, `gpu_device.cpp:72`, `light_table.h:101`, `environment.h:79` | Process-wide, "last Activate wins"; Hydra rebuilds the scene on change | **Renderer** (immutable `std::shared_ptr<const ColorManagement>`). Scenes keep the renderer's pointer; render-time reads take it from the scene, not a global. Changing colour = `Renderer::setColorManagement` which marks every scene for reload (Hydra's behaviour today). |
| 8 | `OCIO` env | `color_management.cpp:111` | Read in `Create` | **Renderer** option `color.config`; `RendererOptions::FromEnvironment()` fills it from `OCIO`. |
| 9 | SSS fallback | `sssFallbackSetting` `src/materialx/loader.cpp:1486-1499`; default arg `loader.h:23`; set `standalone_cli.cpp:911`, `scene_bridge.cpp:5224,5571`; env `PRISM_SSS_FALLBACK` (`loader.cpp:1492`, `render_defaults.h:65`) | Process-wide atomic | **Renderer** material option (`MaterialOptions::sssFallback`), passed to `DocumentLoader` explicitly; part of every material cache key (verify, 7.3 risk). |
| 10 | Material binding purposes | `_BindingPurposes()` `src/hydra/scene_bridge.cpp:356-360`, read `:367`, set `:5501` | Process-wide atomic in Hydra | **Front end**: `prism::usd::LoadOptions::bindingPurposes` (the CLI already passes them through `USDMesh::LoadOptions`, `standalone_cli.cpp:927`); Hydra keeps them per delegate. |
| 11 | Env defaults for scene conversion | `FromEnvironment()` `src/render_defaults.h:53-68`: `PRISM_SUBDIVISION_LEVEL`, `_MAX_FACES`, `PRISM_DISPLACEMENT_LEVEL`, `_MAX_FACES`, `PRISM_SUBDIVISION_BUDGET_FACES`, `PRISM_GPU_TEXTURE_BUDGET_MIB`, `PRISM_SSS_FALLBACK` | Static, read once | **Front end**: `prism::defaults::FromEnvironment()` stays as a helper that front ends call; the core never reads the env for API settings. |
| 12 | MaterialX library and MDL paths | `PRISM_MATERIALX_LIBRARY_PATH`, `PRISM_MATERIALX_MDL`: `standalone_cli.cpp:913`, `scene_bridge.cpp:4643-4644,6159-6162`, `material_service.cpp:846-847` (`EnvOr` `:271-277`) | Env at each use | **Renderer** options `materialx.libraries`, `materialx.mdlPath`; default from env, else a path relative to the library (for packaging). |
| 13 | Cache roots (three different ones) | `PersistentCacheRoot` `src/resource_limits.h:17-24`; material cache `material_service.cpp:851-856` (temp fallback `prism-rt/materials`); `ShaderCacheDirectory` `src/device/gpu/hlsl_compiler.cpp:125-140` (`LOCALAPPDATA/PrismRT/<device>`, `HOME`); `PRISM_CACHE_DIR`, `LOCALAPPDATA`, `HOME` | Env at each use | **Renderer** option `cacheDirectory` (one root, fixed subdirectories). Runtime-shared caches key by root. |
| 14 | Disk cache budget | `PRISM_DISK_CACHE_GB` `resource_limits.h:30-40` | Env at each use | **Renderer** `limits.diskCacheGiB`. |
| 15 | Resource limits (`ResourceLimit`) | `PRISM_MATERIAL_CACHE_MB` `material_service.cpp:109`; `PRISM_PREPARATION_MB` `resource_limits.h:110`; `PRISM_MATERIAL_THREADS` `:124`; `PRISM_TEXTURE_THREADS` `:136`; `PRISM_TEXTURE_CACHE_MB` `texture_service.cpp:131`; `PRISM_TEXTURE_OPEN_FILES` `:151`; `PRISM_TEXTURE_PREPARE_MB` `:196`; `PRISM_TEXTURE_WRITER_THREADS` `:817` (static) | Env, some cached in statics | **Renderer** `RendererOptions::Limits` (env defaults). Process-wide admission budgets (rows 42-43) take the largest request. |
| 16 | Material compile policy | `PRISM_MDL_COMPILE_MODE` `material_service.cpp:192-205`; `PRISM_MATERIAL_PREWARM` `:660`; `PRISM_TEXTURE_PREPARE` `:500`; `PRISM_MDL_RESOURCE_MODE` `src/mdl/backend_compiler.cpp:635`; `PRISM_DISABLE_GENERATION_CACHE` `src/materialx/mdl_generator.cpp:292` | Env | **Renderer** `MaterialOptions` (advanced), env defaults. |
| 17 | Texture writing | `PRISM_TEXTURE_COMPRESSION` `src/texture_writer.cpp:190`; `PRISM_TEXTURE_BACKGROUND_WRITES` `texture_service.cpp:872` | Env | **Renderer** `TextureOptions`. |
| 18 | Device selection and disabling | `PRISM_DEVICE` `scene_bridge.cpp:4619`; `PRISM_DISABLE_VULKAN` `vulkan_device.cpp:3426`; `PRISM_DISABLE_METAL` `metal_device.mm:676` | Env | `PRISM_DEVICE`: **front end** (Hydra default). `PRISM_DISABLE_*`: **Renderer** `devices.disabled`, env default (used by tests). |
| 19 | GPU device tuning (developer knobs) | Vulkan: `PRISM_GPU_TEXTURE_COPIES` `vulkan_device.cpp:171`, `_TEXTURE_BLIT_MIPS` `:182` (static), `_TEXTURE_STREAMING` `:194`, `StreamingSetting` `:199-227` (`PRISM_GPU_STREAMING_TAIL`, `_RESTART_SAMPLES`, `_FEEDBACK_STRIDE`, `_LOD_MARGIN`, `_THRESHOLD`, `_BATCH_MIB`), `_STREAMING_FEEDBACK` `:3320` (static), `_STREAMING_SETTLE_SECONDS` `:1910`, `PRISM_GPU_DENOISER` `:847`, `PRISM_VULKAN_EARLY_IMAGES` `:1281`, `_PROGRAM_LAYOUT` `:2089`, `_RETRY_LARGE` `:413,2150`, `_PREWARM_EMITTERS` `:590` (static). `vulkan_material_calls.cpp`: `PRISM_DXC_JOBS` `:162`, `PRISM_VULKAN_COMPILE_THREADS` `:176`, `_PARALLEL_PIPELINE` `:217`, `_PIPELINE_CACHE` `:224`, `_TRIM_CALLABLES` `:465`, `_SPLIT_PROGRAM_KIB` `:582`, `_SHADER_SALT` `:692`, `_CALLABLES` `:699`, `_MAX_SPIRV_MIB` `:707`, `_DRIVER_MEMORY_MIB` `:775`, `_CALLABLE_THRESHOLD_KIB` `:817`, `_REORDER` `:824`, `_PIPELINE_LIBRARIES` `:830`, `_PIPELINE_STATS` `:836`. Shared GPU: `PRISM_GPU_STREAMING_SETTLE_ROUNDS`/`_MIN_ROUNDS` `gpu_device.cpp:492,506` (static), `PRISM_GPU_MERGE_PROGRAMS` `gpu_materials.cpp:1290` (static), `PRISM_DXC` `hlsl_compiler.cpp:115`, `PRISM_DXC_MEMORY_MIB` `:33` | Env, several frozen in function statics on first use | **Session/device**: `DeviceOptions::advanced` (string map) seeded from env at device creation; values frozen in statics become device fields. Not individual public API. |
| 20 | Varying-emitter triangle limit | `PRISM_MAX_VARYING_EMITTER_TRIANGLES` `src/device/light_table.h:77-85` (static) | Env, frozen | **Session** advanced setting. |
| 21 | Shader source override | `PRISM_SHADER_DIR` `src/device/gpu/shader_sources.cpp:13` | Env | **Stays global** (developer override of the embedded shaders); also `RendererOptions::shaderDirectory`. |
| 22 | Hydra-only env | `PRISM_GPU_PREPARATION_PREVIEW`, `_MAX_MIB`, `_DELAY_MS`, `_THREADS`, `PRISM_PROFILE_BUFFERS` `scene_bridge.cpp:4630-4640` | Env in `Impl` ctor | **Front end** (Hydra defaults) feeding `ProgressiveOptions` of the session. |

### 2.3 Shared caches and registries

Most are content-addressed and safe to share between renderers if their keys
include everything that changes their content (colour identity, MaterialX
library path, SSS fallback, MDL context). That audit is a migration task
(7, step 1d).

| # | State | Where | Today | Target |
|---|---|---|---|---|
| 23 | Shared MDL generators | `MDLGenerator::shared` `src/materialx/mdl_generator.cpp:570-579` | Static map by library path | **Runtime** (stdlib is immutable); reached through the Renderer's material system. |
| 24 | Idle worker generators + thread-local owners | `Idle()` `mdl_generator.cpp:588-594`; `thread_local WorkerGenerators` `:613` | Leaked static pool | **Runtime** (thread-local stays; pool keyed by path). |
| 25 | MaterialX standard library documents + digests | `SharedStandardLibrary` `mdl_generator.cpp:31-47`; `SharedLibraryDigest` `:49-56` | Static maps | **Runtime**. |
| 26 | Colour-transform libraries | `ColorTransformLibrary` `mdl_generator.cpp:88-90` | Static map keyed by stdlib + colour identity | **Runtime** (already keyed by colour). |
| 27 | Colour templates (generated-MDL fast path) | `colorTemplates`, `templateMutex`, `templateKeyMutexes`, `templateBytes`, `templateClock` `mdl_generator.cpp:173-177` | Namespace globals | **Runtime**; key must include colour identity and SSS fallback (verify). |
| 28 | Compiled material cache | `gMaterialCache`, `gMaterialCompiling`, `gCacheClock`, `gCacheEvictions` `material_service.cpp:92-106`; trim `:108-129` | Leaked global (SDK handles) | **Runtime** (budget = largest renderer limit). |
| 29 | Early material cache + association generations | `gEarlyMaterialCache`, `gEarlyCacheReloadGeneration`, `gAssociationReloadGenerations`, `gAssociationGenerations`, `gAssociationScopeUsers` `material_service.cpp:155-161` | Globals keyed by `SdfPath`, `ArResolverContext` | **prism_usd module state** (USD-path keyed; scopes are already per service). |
| 30 | Conversion artifacts | `gConversionArtifacts` `material_service.cpp:173` | Global, USD-keyed | **prism_usd**. |
| 31 | hdMtlx stdlib lock | `HdMtlxLibraryMutex()` `material_service.cpp:350-354` | Static `shared_mutex` | **Stays global** in prism_usd: it guards hdMtlx's own process-wide stdlib. |
| 32 | Material pre-warm registry | `gPrewarms`, `gPrewarmMutex`, `gPrewarmJobs` `material_service.cpp:671-673` | Globals | **Runtime** (keyed by context + paths). |
| 33 | Captured material registry | `CapturedMaterialRegistry::Shared` `src/materialx/captured_material.h:326-329` | Leaked static | **prism_usd** (it captures Hydra material graphs). |
| 34 | Compatible template registry | `CompatibleTemplateRegistry::Shared` `src/materialx/prepared_template.h:393-396` | Leaked static, key has colour identity `:402,438` | **Runtime**. |
| 35 | Resolved resource registry | `GetRegistry()` `src/materialx/resolved_resource.h:59` | Leaked static | **Runtime**. |
| 36 | Generated-MDL disk cache locks | `publicationMutexes`, `pruneMutex` `src/materialx/generated_cache.cpp:30-31` (+ cross-process named lock) | Globals | **Runtime** (per cache root). |
| 37 | Target-code cache, program jobs, capabilities | `gTargetCodeCache`, `gProgramJobs`, `gCapabilities` `src/mdl/backend_compiler.cpp:24-37` | Leaked globals (SDK handles) | **Runtime**. |
| 38 | MDL class observer | `gClassObserver` `backend_compiler.cpp:122-128`, `SetClassObserver` `:446-450` (set by `gpu_materials.cpp:1213,1220`) | Single global observer | **Runtime** event hub (several listeners). |
| 39 | Canonical module mutexes | `gCanonicalModuleMutexes` `src/mdl/material_compiler.cpp:117` | Global | **Stays global** (Runtime): guards names in the process-wide MDL database. |
| 40 | BSDF data tables | `tables` `src/mdl/texture_runtime.cpp:166-167` | Static weak map | **Runtime**. |
| 41 | Texture service | `TextureService::Acquire` weak singleton `src/texture_service.cpp:58-59,964-971`; callers `texture.h:101,110,132`, `texture_runtime.cpp:131,265,423`, `udim_resource.h:150`, `captured_material.h:222,241`, `resolved_resource.h:170,219`, `vulkan_device.cpp:1344,2475`, `metal_device.mm:328`, `scene_bridge.cpp:5093`, `standalone_cli.cpp:1295` | Weak singleton: shared while someone holds it | **Renderer** (`Renderer::Impl::textures`), passed explicitly; Runtime shares one instance between renderers with the same cache root and limits. |
| 42 | Texture preparation budget | `PreparationBudget::instance` `texture_service.cpp:162-166` | Static | **Runtime** (memory admission is process-wide by nature). |
| 43 | Preparation admission | `PreparationAdmission::Shared` `src/resource_limits.h:104` | Static | **Runtime** (same reason). |
| 44 | Prepared-texture observers | `gPreparedObservers`, `gNextPreparedObserver` `texture_service.cpp:984-987` | Globals | **TextureService instance** (so a device only hears its renderer's textures). |
| 45 | Texture pin/release counters | `texturePinSerial`, `releasedResources` `texture_service.cpp:60-62` | Globals | **TextureService instance**. |
| 46 | GPU structural keys | `gStructuralKeys` `src/device/gpu/gpu_materials.cpp:419-421` | Leaked global | **Runtime**. |
| 47 | GPU shader-code caches | `gShaderCodeCache` `gpu_materials.cpp:728-745`; `CachedProgramShaderCode` statics `:1477-1485` | Globals with MDL shutdown hooks | **Runtime**. |
| 48 | GPU program translations + listeners | `gPrograms`, `gProgramListeners`, `gNextListener` `gpu_materials.cpp:866-871`; `AddProgramListener` `:1207-1220` | Globals | Translations: **Runtime** (keyed by context). Listeners: **per device**, through the Runtime hub (row 38). |
| 49 | Stale program files | `gStaleProgramFiles` `gpu_materials.cpp:1006` | Leaked global | **Runtime**. |
| 50 | Environment sampling tables | `SharedEnvironmentTables` `src/device/gpu/gpu_scene.cpp:252-253` | Static 2-entry cache keyed by storage pointer | **Session** (device-owned); a pointer key across renderers is fragile. |
| 51 | Failed Vulkan kernels memo | `failed` `src/device/vulkan/vulkan_device.cpp:400-401` | Static | **Runtime** (keyed by cache file). |
| 52 | Kernel base size | `KernelBaseBytes` `vulkan_device.cpp:451-456` | Static | **Stays global** (pure function of the embedded shaders). |
| 53 | DXC/driver compile admission | `static DxcSlots slots` `vulkan_material_calls.cpp:207`; `Compiles()` `:749-753`; `CommitLimit` `:716-720` | Statics | **Stays global** (Runtime): limits processes and memory machine-wide. |
| 54 | `vkGetRayTracingShaderGroupStackSizeKHR` pointer | `static auto groupStackSize` `vulkan_material_calls.cpp:1106-1107` | Static from the **first** `VkDevice` | **Per device**. Latent bug: a second `VkDevice` in the process (Hydra device switch, Python sessions) reuses the first device's function pointer. Fix in step 1d. |

### 2.4 Events and progress

| # | State | Where | Today | Target |
|---|---|---|---|---|
| 55 | Varying-emitter signal | `detail::state()` `src/varying_emitter_signal.h:31` (`Note`, `Subscribe`); noted by `mdl_material.h:71`; subscribed `vulkan_device.cpp:1255` | Leaked static; a note from one scene stays for the next | **Renderer** (scoped signal). |
| 56 | Current progress model | `Progress::s_current` `src/progress.h:86,202`; free functions `progress::Begin/End/...` `:209-221`; installed by `standalone_cli.cpp:774`, `scene_bridge.cpp:4979`; read `standalone_cli.cpp:711` | "Most recently created wins" | **Renderer/Session**: services get a `Progress*` from their renderer; the free functions stay for deep code, routed through a scope that the renderer installs on its arena threads (`ProgressScope`). Until done: documented one-active-renderer limitation. |
| 57 | Error material | `ErrorMaterial::Shared` `src/material.h:218` | Static | **Stays global** (stateless magenta constant). |

### 2.5 Diagnostics and logging

| # | State | Where | Today | Target |
|---|---|---|---|---|
| 58 | `PRISM_MDL_VERBOSE` | `loader.cpp:20`, `mdl_generator.cpp:27`, `backend_compiler.cpp:133`, `mdl/context.cpp:18`, `material_compiler.cpp:19`, `texture_runtime.cpp:475`, `mdl_material.h:38`, `usd_mesh.h:143` | Env at each use | **Renderer** log level (`LogLevel::Debug` for "mdl"); env stays as the default. |
| 59 | GPU/light verbosity | `PRISM_GPU_VERBOSE` `gpu_device.cpp:45,316` (static), `metal_device.mm:58`; `PRISM_VERBOSE_LIGHTS` `light_table.h:467` (static) | Env | **Renderer** log level per category. |
| 60 | Load profile | `load_profile.cpp:27,37,47` (`PRISM_LOAD_PROFILE`, origin at process start) | Statics | **Stays global** (diagnostic timeline of the process). |
| 61 | Render profiling counters | `inline thread_local active, shadow` `src/render_profile.h:19-20` | Thread-local | **Stays** (opt-in benchmark instrumentation). |
| 62 | Warn-once sets | `lights/ies.cpp:121-122`, `scene_bridge.cpp:105-106,2902`, `material_service.cpp:198,442-443,504`, `udim_resource.h:65` | Statics | **Renderer** logger (dedupe per renderer). Output today goes to `std::cerr` or `TF_WARN`; the logger gets a sink callback (Hydra routes to `TF_WARN`, Python to `logging`). |
| 63 | Debug and test hooks | `PRISM_DUMP_CAMERA_RAYS` `camera_ray_diagnostics.h:19`; `PRISM_GPU_DEBUG` `gpu_device.cpp:75`; `PRISM_GPU_FAIL_DISPATCH_AT` `:123`; `PRISM_GPU_DUMP_PROGRAMS` `gpu_materials.cpp:517`, `vulkan_material_calls.cpp:626,680`; `PRISM_VULKAN_CHECK_ESTIMATE` `vulkan_device.cpp:2102`; `PRISM_GPU_PROGRAM_REPORT` `standalone_cli.cpp:1106`, `scene_bridge.cpp:4796`; `PRISM_MATERIAL_TEMPLATE_DEBUG` `material_service.cpp:84`; `PRISM_TEST_GPU_PREPARATION_DELAY_MS` `scene_bridge.cpp:4804`; `PRISM_TEST_GPU_PREVIEW_MAX_SAMPLES` `:7075`; `PRISM_TEST_TEXTURE_WRITE_FAIL_AFTER` `texture_writer.cpp:34` | Env | **Stays global** (env): test and debug hooks, not API. |

### 2.6 Thread-locals, id counters and constants (stay)

| # | State | Where | Target |
|---|---|---|---|
| 64 | Shading thread-locals | `NativeTextureRuntime::viewOrigin`, `primvarContext` `src/mdl/texture_runtime.h:99,102`; error strings `vulkan_device.cpp:266`, `texture_service.cpp:320` | **Stay**: set per shade call / per error; safe with several sessions. |
| 65 | Unique-id counters | `fingerprint_records.h:140`, `cached_document.h:31`, `resolved_resource.h:183,337`, `texture_gpu_file.cpp:340`, `texture_service.cpp:1520,1801`, `gpu_materials.cpp:949`, `vulkan_material_calls.cpp:508`, `backend_compiler.cpp:1009`, `gResourcePublicationId` `material_service.cpp:88` | **Stay**: temp-file names and ids only need uniqueness. |
| 66 | Immutable tables | `color_management.cpp:66`, `loader.cpp:50,77`, `gltf_material.cpp:307,319,334`, `obj_material.cpp:42`, `resolved_resource.h:143`, `material_service.cpp:332`, `prepared_template.h:101`, `mesh_conversion.cpp:46,416`, `usd_mesh.h:124,321`, `obj_mesh.h:300,370`, `scene_bridge.cpp:211,1261,3076`, `native_tasks.cpp:214`, `texture_runtime.cpp:276,512`, `image_writer.cpp:125-127`, `vulkan_device.cpp:76`, `gpu_materials.cpp:378,852` (regexes), `aov.h:49` | **Stay**: constants. |

### 2.7 Settings that live on the wrong object

Not globals, but the same problem for the API: render settings stored in the
scene, and scene-build options stored with render settings.
- `SceneManager` holds render settings: `rr_min_depth`, `max_depth`,
  `environmentSampling`, `textureFiltering`, `textureStreaming`,
  `gpuTextureBudgetMiB`, `honorDoubleSided` (`src/scene.h:395-412`). The GPU
  devices copy them into scene data, so Hydra treats a change as an edit
  (`scene_bridge.cpp:4987-4990`, `:6645`). They move to `RenderSettings`; the session
  passes them to the device.
- `hydra::RenderSettings` (`src/hydra/scene_bridge.h:34-120`) mixes render
  settings (samples, depth, denoise), scene-build options (subdivision,
  displacement, sphere batching, hidden-geometry budget), renderer options
  (colour, SSS fallback, binding purposes) and Hydra UI options (preview
  sizes, refinement delay). They split into `RenderSettings`,
  `SceneOptions`/loader options, `RendererOptions` and Hydra-only options.
- The CLI's `Options` (`standalone_cli.cpp:66-110`) mirrors the same split.

## 3. Public API sketch (C++)

Namespace `prism`, headers under `include/prism/`. Types are pimpl classes or
plain structs; no third-party types. Sketches, not final signatures.

### 3.1 Object model

```
Runtime (internal, process-wide, ref-counted)
  MDL SDK, shared caches (2.3), admission budgets, OIDN CPU device
Renderer  (public context; several per process allowed)
  options, colour management, material system, texture service,
  Embree device, task arena, logger, progress hub
  Scene     (several per renderer)  handles: Mesh, Spheres, Instance,
                                    Material, Light, Environment, Camera
  Session   (several per renderer)  device + RenderSettings + film (AOVs);
                                    renders committed revisions of one Scene
```

Ownership: `Renderer`, `Scene`, `Session` are cheap handles to ref-counted
implementations (`std::shared_ptr<Impl>`), like OIDN's `DeviceRef`. A scene
keeps its renderer alive; a session keeps its scene and renderer alive, so
Python objects never dangle.

### 3.2 Common types (`prism/types.h`)

```cpp
namespace prism {
// The renderer's own math types (src/float2.h, float3.h, float4.h) become
// public as they are: std-only. Color3 is a Float3.
using ::Float2; using ::Float3; using ::Float4; using Color3 = Float3;
// Row-major 4x4 for column vectors (p' = M p). Double for transforms.
struct Matrix4d { std::array<double, 16> m; static Matrix4d identity(); };

// A typed view of caller memory. With `owner` set, the core may keep a
// reference instead of copying (zero-copy; Python passes the ndarray owner).
enum class DataType : uint8_t { Float32, Float64, Int32, UInt32, UInt8 };
struct BufferView {
    const void* data = nullptr;
    size_t count = 0;              // elements
    uint32_t components = 1;       // per element (3 for points)
    DataType type = DataType::Float32;
    size_t strideBytes = 0;        // 0: tightly packed
    std::shared_ptr<const void> owner;   // optional keep-alive
};

// Time samples (section 5). One sample = not animated.
template <class T> struct TimeSample { double time = 0.0; T value; };
template <class T> using Sampled = std::vector<TimeSample<T>>;

enum class LogLevel { Debug, Info, Warning, Error };
}
```

### 3.3 Errors

Proposal (open question 8.1): exceptions, as the code does today
(`std::runtime_error` everywhere).
- `prism::Error : std::runtime_error` with `ErrorCode code()`:
  `InvalidArgument`, `InvalidHandle`, `NotFound` (file), `Unsupported`
  (feature or device not in this build), `DeviceLost`, `OutOfMemory`,
  `Cancelled`, `Internal`.
- Non-fatal problems are diagnostics, not errors: a material that fails to
  compile renders with the magenta error material and adds a
  `Diagnostic{level, category, message, handle}` to `Scene::diagnostics()` and
  the log. Same behaviour as today (`ErrorMaterial`, warnings on `std::cerr`).
- Errors on render threads are kept by the session and rethrown by `wait()` /
  `render()`; `status()` reports them without throwing.
- nanobind maps `prism::Error` to Python exceptions (`prism.Error`, a
  subclass per code; `Cancelled` → `prism.CancelledError`).

### 3.4 Renderer (`prism/renderer.h`)

```cpp
namespace prism {
struct ColorOptions {              // today prism::color::Options
    std::string config;            // empty: studio config
    std::string workingSpace;      // empty: ACEScg
    std::string display, view;
};
struct MaterialOptions {
    std::string materialxLibraries;   // PRISM_MATERIALX_LIBRARY_PATH
    std::string materialxMdlPath;     // PRISM_MATERIALX_MDL
    bool sssFallback = false;         // was DocumentLoader::SetSssFallback
    enum class CompileMode { Auto, Class } compileMode = CompileMode::Auto;
    bool prewarm = true;
};
struct Limits {                       // 2.2 rows 14-15; 0 = built-in default
    uint64_t materialCacheMiB = 0, preparationMiB = 0, textureCacheMiB = 0,
             texturePrepareMiB = 0, diskCacheGiB = 0;
    unsigned materialThreads = 0, textureThreads = 0, textureWriterThreads = 0,
             textureOpenFiles = 0;
};
struct RendererOptions {
    ColorOptions color;
    MaterialOptions materials;
    Limits limits;
    std::string cacheDirectory;       // empty: the platform cache directory
    unsigned threads = 0;             // task arena size; 0: all cores
    std::vector<DeviceKind> disabledDevices;   // PRISM_DISABLE_VULKAN/METAL
    std::function<void(LogLevel, std::string_view category, std::string_view)> log;
    LogLevel logLevel = LogLevel::Warning;
    // The front ends' defaults: each field from its env var (OCIO, PRISM_*).
    static RendererOptions FromEnvironment();
};

class Renderer {
public:
    explicit Renderer(RendererOptions const& = {});
    RendererOptions const& options() const;

    // Colour management. Changing it marks every scene of this renderer for
    // reload (materials, lights, textures), as the Hydra delegate does today.
    ColorInfo color() const;          // working space, display, view, config name
    void setColor(ColorOptions const&);

    std::vector<DeviceInfo> devices() const;   // today AvailableDevices(), CPU last
    Capabilities capabilities() const;         // denoiser, devices, motionBlur
    Scene createScene(SceneOptions const& = {});
    Session createSession(Scene const&, SessionOptions const& = {});

    RendererStats stats() const;      // texture + material counters (the CLI's [PrismProfile])
};
}
```

Notes:
- `RendererOptions::FromEnvironment()` replaces the scattered `getenv` reads
  of 2.2. The CLI and Hydra call it; Python by default too (open question 8.6).
- Heavy members (texture service, material system, Embree device) are created
  on first use, so `devices()` stays cheap.

### 3.5 Scene (`prism/scene.h`)

```cpp
namespace prism {
// Handles: 32-bit slot + 32-bit generation, typed value types. A removed
// handle throws InvalidHandle.
struct MeshId { uint64_t v; };     struct SpheresId { uint64_t v; };
struct InstanceId { uint64_t v; }; struct MaterialId { uint64_t v; };
struct LightId { uint64_t v; };    struct EnvironmentId { uint64_t v; };
struct CameraId { uint64_t v; };

enum class Interpolation { Constant, Uniform, Vertex, FaceVarying, Instance };
struct Primvar {                   // named data for MDL scene-data lookups
    std::string name; Interpolation interpolation; BufferView data;
};

struct SubdivisionDesc {           // today prism::mesh::SubdivisionSettings + USD tags
    enum class Scheme { None, CatmullClark, Loop, Bilinear } scheme = Scheme::None;
    int level = -1;                // -1: SceneOptions default
    BufferView creaseIndices, creaseLengths, creaseSharpness,
               cornerIndices, cornerSharpness, holeIndices;
};

struct MeshDesc {
    Sampled<BufferView> points;          // section 5: one sample today
    BufferView faceVertexCounts;         // empty: all triangles
    BufferView faceVertexIndices;
    BufferView normals;  Interpolation normalsInterpolation = Interpolation::Vertex;
    BufferView uvs;      Interpolation uvsInterpolation = Interpolation::FaceVarying;
    BufferView tangents;
    std::vector<Primvar> primvars;
    BufferView velocities, accelerations;   // reserved (section 5)
    SubdivisionDesc subdivision;
    // Per-face material slots (USD GeomSubsets, glTF primitives, OBJ usemtl):
    // faceMaterialSlots[i] indexes InstanceDesc::materials.
    BufferView faceMaterialSlots;
    bool doubleSided = true;
    BufferView singleSidedFaces;         // per-face flags (the CLI's merged meshes)
};

struct SpheresDesc {                     // analytic spheres (Sphere, BatchedSpheres)
    Sampled<BufferView> centers;  BufferView radii;
};

struct InstanceDesc {
    std::variant<MeshId, SpheresId> prototype;
    Sampled<Matrix4d> transform{{0.0, Matrix4d::identity()}};
    std::vector<MaterialId> materials;   // slot 0 is the default
    std::vector<Primvar> primvars;       // instance-rate (InstancePrimvars)
    bool visible = true;                 // Hydra visibility (disabled_placements)
    std::vector<std::string> lightLinkCategories, shadowLinkCategories;
    // Reported by the primId/instanceId AOVs and by pick(): the front end's
    // numbering (Hydra prim ids; the CLI numbers placements like Hydra).
    int32_t primId = -1, instanceId = -1;
    uint64_t userData = 0;               // opaque, returned by pick()
};

struct LightDesc {                       // today prism::LightParameters
    LightShape shape;                    // Sphere, Rect, Disk, Cylinder, Distant, Point
    Sampled<Matrix4d> transform{{0.0, Matrix4d::identity()}};
    Color3 color{1.0f};                  // authored, in colorSpace
    std::string colorSpace = "lin_rec709";
    float intensity = 1, exposure = 0, diffuse = 1, specular = 1;
    bool normalize = false, cameraVisible = false;
    std::optional<float> temperature;    // enableColorTemperature
    float radius = 0.5f, width = 1, height = 1, length = 1, angle = 0.53f;
    bool treatAsPoint = false;
    LightShaping shaping;                // cone, focus, IES file
    std::string texture, textureColorSpace;   // rect lights
    std::string lightLinkCategory, shadowLinkCategory;
};

struct EnvironmentDesc {                 // USD dome, --env, --env-color
    std::string texture;                 // empty: constant colour
    std::string textureColorSpace;
    Color3 color{1.0f}; float intensity = 1, exposure = 0;
    float diffuse = 1, specular = 1;
    Sampled<Matrix4d> transform{{0.0, Matrix4d::identity()}};
    bool cameraVisible = true;           // domeLightCameraVisibility
};

struct CameraDesc {
    Sampled<Matrix4d> cameraToWorld{{0.0, Matrix4d::identity()}};
    enum class Projection { Perspective, Orthographic } projection{};
    float focalLength = 50, horizontalAperture = 36, verticalAperture = 24,
          horizontalApertureOffset = 0, verticalApertureOffset = 0;
    float nearClip = 0.001f, farClip = 1e30f;
    float fStop = 0, focusDistance = 1;
    float exposureScale = 1;             // UsdGeomCamera::ComputeLinearExposureScale
    std::optional<Shutter> shutter;      // reserved (section 5)
    // For Hydra: explicit matrices (HdCamera, pass camera).
    std::optional<Matrix4d> worldToView, projectionMatrix;
};

struct SceneOptions {                    // build options (were render settings)
    int subdivisionLevel = 2, subdivisionMaxFaces = 1 << 16,
        subdivisionBudgetFaces = 24 << 20;
    int displacementLevel = 4, displacementMaxFaces = 1 << 18;
    bool sphereBatching = false;
    uint64_t hiddenGeometryBudgetMiB = 0;
    bool defaultDome = false;            // usdview's dome when nothing emits
    double timeCodesPerSecond = 24;      // velocities (section 5)
};

class Scene {
public:
    MeshId addMesh(MeshDesc const&);
    void updateMesh(MeshId, MeshDesc const&);                    // full replace
    void updateMeshPoints(MeshId, Sampled<BufferView> const&);   // deformation only
    SpheresId addSpheres(SpheresDesc const&);
    InstanceId addInstance(InstanceDesc const&);
    void setTransform(InstanceId, Sampled<Matrix4d> const&);
    void setVisible(InstanceId, bool);
    void setMaterials(InstanceId, std::vector<MaterialId> const&);
    MaterialId addMaterial(MaterialDesc const&);                 // 3.6
    void updateMaterial(MaterialId, MaterialDesc const&);
    LightId addLight(LightDesc const&);     void updateLight(LightId, LightDesc const&);
    EnvironmentId addEnvironment(EnvironmentDesc const&);       // several: composite
    CameraId addCamera(CameraDesc const&);  void updateCamera(CameraId, CameraDesc const&);
    template <class Id> void remove(Id);

    // Publishes the edits since the last commit as a new revision: converts
    // meshes (subdivision, displacement), compiles new materials, prepares
    // textures, builds the Embree scene. Sessions take the new revision at
    // their next pass. Blocking; the callback can cancel it.
    CommitResult commit(ProgressCallback const& = {});
    uint64_t revision() const;
    std::vector<Diagnostic> diagnostics() const;
    SceneStats stats() const;            // meshes, triangles, materials, memory
};
}
```

Edit semantics:
- Edits are recorded, not applied: nothing a session renders changes before
  `commit()` (Embree's model; Hydra's snapshot publication today:
  `Snapshot` and the spare "banks" in `scene_bridge.cpp`).
- `commit()` builds the next revision beside the published one and swaps it
  in. Sessions keep rendering the old revision meanwhile and restart
  accumulation on the swap. The Hydra bridge already does this with its
  double-buffered snapshots; that machinery moves into the core (7, step 5).
- `commit()` classifies edits with the dirty kinds Hydra uses today
  (`SceneDirtyKind`, `scene_bridge.h:116-124`: placement, prototype,
  material, environment, structure), so a transform edit refits instances and
  does not convert prototypes, and a light edit does not rebuild the BVH.
- `CommitResult` says what changed so sessions restart only what they must
  (GPU: re-upload light table vs full scene).

Threading rules:
- One writer per `Scene` at a time (externally synchronized, as Embree
  objects). Edits may run on any thread, also while sessions render.
- `commit()` must not overlap edits of the same scene; it may run while
  sessions render the previous revision.
- `Renderer` methods and read-only queries are thread-safe.
- Callbacks (progress, log) run on internal threads and must not edit the
  scene that is committing.

### 3.6 Materials (`prism/material.h`)

```cpp
namespace prism {
struct MaterialDesc {
    enum class Kind { MaterialXFile, MaterialXString, Diffuse, Emissive } kind;
    std::string path;            // .mtlx file
    std::string document;        // .mtlx XML text
    std::string materialName;    // empty: the first material of the document
    std::string searchPath;      // base for relative textures of a string
    Color3 color{0.18f};         // Diffuse / Emissive
    float opacity = 1.0f;
    // Overrides of exposed inputs: runtime arguments where the compiled
    // program has them (template rebinding), else a recompile.
    std::vector<std::pair<std::string, ParameterValue>> parameters;
};
}
```
- Built-ins map to `Diffuse` (the displayColor/displayOpacity fallback),
  `Emissive` and the error material (`src/material.h:136,180,213`).
- MaterialX strings: `MDLMaterial` compiles only from a file path today
  (`src/mdl_material.h:81`). A string is written into the renderer's material
  cache under a content hash (the glTF and OBJ converters already publish
  generated documents there) and compiled from that file. Later
  `DocumentLoader` can take the document in memory.
- `prism/materialx_interop.h` (optional, header-only, needs MaterialX) turns a
  `MaterialX::DocumentPtr` into a string; the core stays free of MaterialX
  types.
- UsdShade networks come through `prism_usd`
  (`prism::usd::addMaterial(scene, stage, path)`), which runs today's
  `MaterialService` on Hydra data sources inside the module and returns a
  `MaterialId`.
- Compilation happens in `commit()` (load time), never in the render loop
  (CLAUDE.md). `commit()` hands the new materials to the material system's
  worker pool in one batch, as `MaterialService::Prefetch` does today.

### 3.7 Render settings, devices, sessions (`prism/session.h`)

```cpp
namespace prism {
enum class DeviceKind { Cpu, Vulkan, Metal };
enum class DevicePreference { Auto, Cpu, Vulkan, Metal };
struct DeviceInfo { DeviceKind kind; std::string name; bool hardwareRayTracing; };

struct Shutter { double open = 0.0, close = 0.0; };   // reserved (section 5)

struct RenderSettings {          // merged hydra::RenderSettings + the CLI's Options
    int samples = 2048;          // defaults from src/render_defaults.h
    float adaptiveThreshold = 0.01f;  int adaptiveMinSamples = 64;
    double timeLimitSeconds = 0;
    int maxDepth = 32, rouletteDepth = 4;
    uint32_t seed = 0;
    float sampleClamp = 0;       // 0: off (Hydra's default is 10)
    bool environmentMis = true;  // environmentSampling mis | bsdf
    TextureFiltering textureFiltering = TextureFiltering::Filtered;
    bool honorDoubleSided = false;
    bool transparentBackground = false;   // SampleShading::transparentEscapes
    NeeSettings nee;             // probabilityMin, fromDepth, spatialLights
    bool textureStreaming = true, textureStreamingPrepass = false;
    int gpuTextureBudgetMiB = 0;
    Shutter shutter;             // reserved (section 5)
    std::map<std::string, std::string> advanced;   // 2.2 rows 19-20; env defaults
};

struct DenoiseSettings { enum class Quality { Fast, Balanced, High } quality; bool prefilter = true; };

enum class Aov { Color, Depth, CameraDepth, Normal, Neye, Peye, Pworld, St,
                 PrimId, ElementId, InstanceId,
                 Albedo, Diffuse, Specular, Emission, Denoised };   // src/output/aov.h:25-31

struct SessionOptions {
    DevicePreference device = DevicePreference::Auto;
    bool allowFallback = true;   // to the CPU device, with a note
    unsigned width = 0, height = 0;
    struct Window { unsigned x, y, width, height; };
    std::optional<Window> dataWindow;      // render products' data window
    std::vector<Aov> aovs{Aov::Color};
    RenderSettings settings;
    std::optional<DenoiseSettings> denoise;   // required by Aov::Denoised
    ProgressiveOptions progressive;        // preview size, refinement delay,
                                           // denoise interval, GPU preparation preview
};

struct Progress {                // today prism::Progress::Snapshot + ImageProgress
    std::string phase;           // device, loading, materials, textures, kernel,
                                 // upload, rendering, idle, error
    double phaseSeconds = 0; std::string detail, device, error;
    double fraction = 0;         // ImageProgress::Fraction()
    double meanSamples = 0; int pass = 0;
};
// Called on an internal thread, rate-limited (default 10 Hz, and at the end
// of each pass). Returning false cancels: the blocking call then throws
// Cancelled, or the progressive session stops.
using ProgressCallback = std::function<bool(Progress const&)>;

class Image {                    // host pixels with shared ownership
public:
    unsigned width() const; unsigned height() const; unsigned channels() const;
    DataType type() const;       // Float32, or Int32 for id AOVs
    const void* data() const;    // row-major, top row first
    std::shared_ptr<const void> owner() const;   // for zero-copy wrappers
};

class Session {
public:
    DeviceInfo device() const;
    std::string deviceNote() const;        // why it fell back to the CPU

    // A change restarts accumulation.
    void setSettings(RenderSettings const&);
    void setCamera(CameraId);
    void setCamera(CameraDesc const&);     // per-pass camera (Hydra)
    void resize(unsigned width, unsigned height, std::optional<Window> = {});

    // Device setup (kernel compile, upload, light tables): today's
    // RenderDevice::Prepare. Optional; rendering calls it.
    void prepare(ProgressCallback const& = {});

    // Batch: to completion (samples, adaptive, time limit) through today's
    // RenderDevice::RenderImage path. Blocking.
    RenderResult render(ProgressCallback const& = {});

    // Progressive: passes on an internal thread; takes new scene revisions
    // between passes.
    void start(ProgressCallback const& = {});
    void pause(); void resume();
    void cancel();                         // asynchronous; wait() returns
    void restart();                        // accumulation from sample 0
    SessionStatus wait(double timeoutSeconds = -1);   // rethrows render errors
    SessionStatus status() const;          // running, converged, paused, error

    // Film snapshots, safe while start() runs (copied at a pass boundary).
    Image aov(Aov) const;
    Image denoised() const;                // denoise now (progressive)
    Pick pick(unsigned x, unsigned y) const;   // instance, primitive, userData, position
};

struct RenderResult {
    std::map<Aov, Image> aovs;
    double meanSamples, fractionAtMaxSamples, renderSeconds, denoiseMilliseconds;
    bool timeLimitReached;
    std::string denoiser;
};
}
```

Notes:
- Two render paths stay, as today: `render()` uses `RenderDevice::RenderImage`
  (the CLI's whole-image path), `start()` the `RenderPass` loop with tiles,
  adaptive `ActivePixels`, previews and publication that lives in
  `scene_bridge.cpp` today (`SceneBridge::Render` `:6612`, passes
  `:7141-7199`, denoise interval `:7418`). Both keep CPU/Vulkan parity and the
  CLI/Hydra parity fixtures.
- Exposure: the camera's `exposureScale` applies after the sample clamp, to
  colour and radiance AOVs, not to albedo (`standalone_cli.cpp:1196-1226`).
  The session does it; front ends stop repeating it.
- Geometric AOVs (depth, normals, ids, Pworld, st) come from pixel-centre
  primary hits on both paths, as today (`standalone_cli.cpp:1234-1270`;
  Hydra's `resolvePrimary`).
- Files are not in the core: `prism_io` (OIIO) writes `Image`s as EXR layers
  or display-encoded 8-bit through the renderer's colour management (today
  `src/output/image_writer.cpp`). Render products/vars come from USD
  (`src/output/render_products.cpp`), so they go to `prism_usd`.
- "One GPU process at a time" is a machine rule, not enforced by the API.

### 3.8 Lifetimes and shutdown
- The last handle to a renderer joins its sessions' threads (sessions hold
  references, so nothing renders on a dead renderer).
- The Runtime lives for the process (the MDL SDK cannot restart). Its caches
  with MDL handles are emptied by the existing MDL shutdown hooks (row 3) at
  exit, not when the last renderer goes.
- Python: an `atexit` hook cancels and joins all sessions before the
  interpreter finalizes, so no render thread touches Python afterwards.

### 3.9 How the front ends map onto the API

CLI (`src/standalone_cli.cpp`, 1317 lines) becomes argument parsing plus:

| CLI today | API |
|---|---|
| `EmbreeDevice`, `color::Activate`, `SharedMdlContext()`, `SetSssFallback`, `getenv("PRISM_MATERIALX_LIBRARY_PATH")` (`:742,862,908-913`) | `Renderer r(options)` with `RendererOptions::FromEnvironment()` + flags |
| `UsdStage::Open`, `ResolveRenderJob` (`:788-794`) | `prism::usd::openStage`, `prism::usd::resolveRenderJob` |
| `USDMesh` / `glTFMesh` / `ObjMesh` + `attachImportedMesh` (`:925-949`) | `prism::usd::load(scene, stage, opts)`, `prism::gltf::load(scene, file)`, `prism::obj::load(scene, file, opts)` |
| UsdLux traversal and domes (`:979-1080`) | inside `prism::usd::load` (`LightDesc`, `EnvironmentDesc`) |
| `--env`, `--env-color`, default dome (`:960-975,1087-1104`) | `scene.addEnvironment(...)`; `SceneOptions::defaultDome` |
| `scene.commitScene()` (`:1083`) | `scene.commit(progress)` |
| `CreateDevice`, `SceneChanged` (`:1113-1119`) | `r.createSession(scene, {device, ...})` |
| `ImageRequest`, `RenderImage`, exposure, light AOVs, denoise (`:1159-1228`) | `session.render(progress)` → `RenderResult` |
| geometric AOV loop (`:1234-1270`) | inside `render()` (requested `aovs`) |
| `BuildProductImage`, `WriteImage` (`:1274-1282`) | `prism::usd::writeProduct` / `prism::io::write` |
| `ProgressPrinter` polling `Progress::Current()` and `ImageProgress` (`:575-740`) | the progress callback |

Hydra (`src/hydra/`):

| Hydra today | API |
|---|---|
| `SceneBridge::Impl` ctor: `rtcNewDevice`, `PRISM_*` env, `MaterialService` (`scene_bridge.cpp:4613-4660`) | a `Renderer` per delegate (or one shared, open question 8.12); `prism::usd` material adapter around `MaterialService` |
| snapshot build/refresh from the scene index, banks, dirty notices (bulk of `scene_bridge.cpp`, ~1300-4400) | scene-index observer turns prims into `Scene` edits + `commit()`; the bank/snapshot machinery moves into `Scene::commit` |
| `SetSettings` (`:5363`), `color::Activate` (`:5219,5376`), `SetSssFallback` (`:5224,5571`), `_BindingPurposes` (`:5501`) | `session.setSettings`, `renderer.setColor`, renderer material options, per-delegate binding purposes |
| render thread: `RenderPreview`, `RenderPass`, adaptive, accumulation, denoise interval (`:6612-7430`) | `session.start()`; `PrismRenderBuffer`s read `session.aov()` snapshots (or a zero-copy frame callback) |
| picking (`PickingTable`, `PublishedFrame` `scene_bridge.h:155-180`; `RenderDevice::Intersect` `device.h:273`) | `session.pick(x, y)` → `instanceId`/`primId`/`userData`; USD path tables stay in Hydra |
| `GetStats`, `GetProgress` (`scene_bridge.h:205-209`) | `session.status()`, the progress callback, `renderer.stats()` |
| GPU preparation preview (CPU renders while the GPU prepares) | `ProgressiveOptions::gpuPreparationPreview` |

## 4. Header split and module boundaries

### 4.1 Modules (CMake targets)

| Target | Contents | Links (private) | Public deps |
|---|---|---|---|
| `prism` (shared library, `PRISM_API` exports) | Renderer, Scene, Session, materials (MaterialX → MDL), devices (CPU, Vulkan, Metal), integrator, colour, textures, denoisers | Embree, oneTBB, MDL SDK (loaded at run time), MaterialX, OIIO, OCIO, OIDN, Vulkan/Metal, OpenSubdiv | std only |
| `prism_io` | image writing (`src/output/image_writer.cpp`) | OIIO | `prism` |
| `prism_usd` | stage loading (`usd_mesh.h`), UsdLux, UsdShade via `MaterialService`, render settings/products, Vt mesh adapter, StageCache lookup | usd, usdGeom, usdLux, usdShade, usdRender, hd (data sources), hdMtlx | `prism` (+ pxr types in `prism/usd.h` only) |
| `prism_gltf` | `gltf_mesh.h`, `gltf_material.cpp` | tinygltf, MaterialX | `prism` |
| `prism_obj` | `obj_mesh.h`, `obj_material.cpp` | tinyobjloader, MaterialX | `prism` |
| `PrismRT` (CLI) | argument parsing, printing | `prism`, `prism_io`, `prism_usd`, `prism_gltf`, `prism_obj` | |
| `hdPrism` (plugin) | `src/hydra/*` | `prism`, `prism_usd`, hd, hdx | |
| `_prism` (Python extension) | nanobind bindings | `prism`, `prism_io`, loaders; `prism_usd` optional | |

`prism` must be a shared library:
- Today `prism_core` is static (`CMakeLists.txt:236`) and linked into both
  `PrismRT` and `hdPrism` (`src/hydra/CMakeLists.txt:11`).
- A Python process that loads `hdPrism` (usdview, or `UsdImagingGL` from a
  script) and `_prism` would hold two copies of every global of section 2,
  including two owners of the one `INeuray`.
- One shared `prism` keeps the Runtime unique per process. `prism_usd` holds
  module state (rows 29-31, 33), so it is shared too when both `hdPrism` and
  `_prism` use it. `prism_gltf`/`prism_obj` can stay static.

### 4.2 Where the current files go

| Current | Target |
|---|---|
| `src/scene.h` (`SceneManager`) | `prism` internal; the public `Scene` wraps it |
| `src/device/*` (`device.h`, `cpu_*`, `gpu/*`, `vulkan/*`, `metal/*`, `denoiser.*`, `adaptive_sampling.h`, `light_table.h`) | `prism` internal; `DeviceInfo`, `DeviceKind`, `DenoiseSettings`, `NeeSettings` become public types |
| `shaders/` | `prism` (embedded) |
| `src/color/*` | `prism`; `ColorManagement` internal, `ColorOptions`/`ColorInfo` public |
| `src/materialx/loader.*`, `mdl_generator.*`, `generated_cache.*`, `cached_document.h`, `prepared_template.h` (minus Hd parts), `resolved_resource.h`, `udim_resource.h` | `prism` internal |
| `src/materialx/material_service.*`, `captured_material.h`, `usd_network.h`, `hdmtlx_names.h`, `preview_surface.h` | `prism_usd` (Hydra data sources) |
| `src/materialx/gltf_material.*`, `obj_material.*` | `prism_gltf`, `prism_obj` |
| `src/mdl/*`, `src/mdl_material.h` | `prism` internal |
| `src/mesh/usd_mesh.h`, Vt part of `mesh_conversion.*` | `prism_usd` |
| `src/mesh/subdivision.*` (std-only), std part of `mesh_conversion.*` | `prism` internal (used by `commit()`) |
| `src/mesh/gltf_mesh.h`, `obj_mesh.h` | `prism_gltf`, `prism_obj` |
| `src/output/aov.h` | `prism` (enum public, `ResolveGeometricAov` internal) |
| `src/output/render_products.*` | `prism_usd` |
| `src/output/image_writer.*` | `prism_io` |
| `src/texture*`, `texture_service.*` | `prism` internal |
| `src/lights.h`, `src/lights/ies.*`, `src/environment.h` | `prism` internal; `LightDesc`/`EnvironmentDesc` public |
| `src/progress.h` | `prism` internal; `Progress` snapshot public |
| `src/render_defaults.h` | public `prism/defaults.h` (already std-only) |
| `src/float2.h`, `float3.h`, `float4.h` | public `prism/math.h` |
| `src/camera_rays.h`, `camera_ray_diagnostics.h` | `prism` internal (after the Gf removal below) |
| `src/standalone_cli.*`, `main.cpp` | `PrismRT` |
| `src/hydra/*` | `hdPrism` |

### 4.3 Concrete offenders

USD (`pxr`) in code the core needs:
- `src/camera_rays.h:6-8,16-17,48-73`: `CameraRayParameters` stores
  `pxr::GfMatrix4d` and does camera math with `GfVec*`. It sits in
  `ImageRequest`/`PassRequest`, so every device depends on USD. Replace with
  `prism::Matrix4d` and own inverse/transform helpers (exact results must not
  change: check `--exact`).
- `src/output/aov.h:13-15`: `GfMatrix4d` in `ResolveGeometricAov`. Same fix.
- `src/mesh/obj_mesh.h:33-35,171-188` and `src/mesh/mesh_conversion.h:12-14`:
  the OBJ loader builds `VtVec3fArray`/`VtValue`/`TfToken` to reuse
  `ConvertMesh` for subdivision. Split `mesh_conversion` into a std-only
  `MeshSource` (spans) used by `commit()` and a Vt adapter in `prism_usd`.
  Then glTF and OBJ need no USD.
- `src/materialx/material_service.h:9-11`: Hd data sources and `SdfPath` in
  the material service API. The service goes to `prism_usd`; its USD-free
  caches (rows 28, 32, 34, 35) go into the core material system.
- `src/scene.h:20-22,58-100`: includes all three loaders;
  `attachImportedMesh` casts to `USDMesh`. Loaders call `Scene` instead.
- `CMakeLists.txt:336-346`: `prism_core` links `${PXR_LIBRARIES}` and puts
  `${PXR_INCLUDE_DIRS}` and `${Python3_INCLUDE_DIRS}` in its PUBLIC include
  path. They move to `prism_usd`.

Embree types in today's front-end-facing headers:
- `src/device/device.h:273`: `RenderDevice::Intersect` returns `RTCRayHit`;
  `PassRequest::resolvePrimary` (`:184`) and `PreviewRequest::writePixel`
  (`:207`) pass `RayHit`, which wraps Embree (`src/ray.h:3,43-45`). They stay
  internal; the public `Session::pick` returns `Pick`.
- `src/ray.h:3,36-39` (`PathRay` holds `RTCRay`), `src/geometry.h:14,28`
  (`setupEmbree(RTCDevice&, RTCScene&)`), `src/scene.h:11-12,46-50,373-378`
  (`RTCDevice&` constructor, `RTCScene`, `RTCGeometry` maps): internal.

MDL SDK, MaterialX, OIIO, TBB:
- `src/mdl_material.h:15` (`mi/mdl_sdk.h`) and `src/mdl/*.h`: internal.
- `src/materialx/mdl_generator.h:3-4`, `loader.h` (MaterialX): internal.
- `src/texture.h:13-16`: OIIO in a header that `material.h:7`,
  `environment.h:4` and `lights.h:9` include, so every internal header pulls
  OIIO. Internal, so not blocking; a pimpl later helps build times.
- `src/texture_service.h` (OIIO): internal.
- TBB is only in `.cpp` files and `light_table.h` (internal). The task arena
  (row 6) lives in `Renderer::Impl`.

Process-wide setters that leave the API:
- `prism::color::Active/SetActive/Activate` (`color_management.h:96-102`).
- `DocumentLoader::SetSssFallback/DefaultSssFallback` (`loader.h:25-29`).
- `TextureService::Acquire` and its static observers
  (`texture_service.h:125,182-185`).
- `MDLGenerator::shared/workerLocal/reserveWorkers` (`mdl_generator.h:38-46`)
  become Runtime members.
- `prism::Progress::Current/Install` (`progress.h:86-87`).

## 5. Motion reservation

Goal: motion blur (Phase 8b) adds behaviour, not API breaks. The shapes are
reserved now; the first SDK release accepts them and renders one instant.

API shapes (in the sketches above):
- Transforms: `Sampled<Matrix4d>` in `InstanceDesc::transform`,
  `LightDesc::transform`, `EnvironmentDesc::transform`,
  `CameraDesc::cameraToWorld`, and `Scene::setTransform(InstanceId,
  Sampled<Matrix4d>)`.
- Deformation: `MeshDesc::points` and `SpheresDesc::centers` are
  `Sampled<BufferView>`; `Scene::updateMeshPoints(MeshId,
  Sampled<BufferView>)`. Topology is not time-sampled (USD's rule).
- Velocities: `MeshDesc::velocities`, `accelerations` (per point, units per
  second), converted with `SceneOptions::timeCodesPerSecond` as USD does.
  Instance velocities (PointInstancer) can be added to `InstanceDesc` the same
  way.
- Other attributes (light colour/intensity, camera focus) are plain values.
  If they need samples later, `Sampled<>` siblings can be added without
  breaking callers.
- Time unit: offsets from the frame time in time codes, the unit of
  `UsdTimeCode` and of Hydra's shutter (`HdCameraTokens->shutterOpen/Close`).
  The frame time itself is the front end's (the CLI's `--time`, Hydra's
  current time).
- Shutter: `RenderSettings::shutter`, overridden by `CameraDesc::shutter`
  when set (USD cameras carry it). Default `{0, 0}`: no motion.
- Samples: sorted, distinct times; times outside the shutter are fine
  (interpolation brackets it); attributes may have different sample counts.

Render path (Phase 8b, from PLAN.md):
- Per-sample time `t = lerp(open, close, u)`, `u` from its own RNG dimension
  (stratified per pixel), fixed along the path.
- CPU: Embree's `RTCRay::time` exists (`PathRay::ray` is an `RTCRay`,
  `src/ray.h:36-39`), normalized over the geometry's time range; Embree time
  steps through `rtcSetGeometryTimeStepCount` and per-step transforms and
  vertex buffers. Nothing sets `time` today (zero-initialized).
- Shared integrator: `Ray` (`shaders/integrator/types.h:145-150`) has no
  time. Add `float time` when Phase 8b starts (it changes the GPU layout, so
  not now); the public API does not depend on it.
- MDL: `state.animation_time` is fixed at 0 (`src/mdl_material.h:698`,
  `shaders/gpu/gpu_backend.hlsl:544`); it becomes the sample time.
- Vulkan: one instant per pass (PLAN.md stage 4), so the internal pass request
  gets a `time` field when the pass loop moves into the session.

Until implemented:
- `Renderer::capabilities().motionBlur == false`.
- With several samples, the core uses the sample nearest to the shutter centre
  (time 0 for `{0,0}`) and logs one warning per scene. That equals today's
  behaviour (both loaders evaluate one time code).
- Python takes the same shapes from day one (6.3), so scripts written now get
  motion blur later without changes.

## 6. Python API sketch (nanobind)

A thin layer: one Python class per C++ class, snake_case names, keyword
arguments for the `*Desc` structs. No logic that the C++ API lacks.

### 6.1 Module layout

```
prismrt/                     # package name: open question 8.3
  __init__.py                # DLL directories, version check, re-exports
  _prism.pyd / .so           # Renderer, Scene, Session, types, gltf, obj, io
  usd.py + _prism_usd.pyd    # needs pxr; imported on first use of prismrt.usd
```
- `prismrt.Renderer`, `Scene`, `Session`, `RenderSettings`, `Aov`,
  `DeviceKind`, `Error` (+ subclasses), `MeshDesc`-style keyword functions.
- `prismrt.gltf.load(scene, path)`, `prismrt.obj.load(scene, path, ...)`.
- `prismrt.usd.load(scene, path_or_stage, time=..., ...)`,
  `prismrt.usd.render_job(stage, settings_path)`,
  `prismrt.usd.write_products(job, result)`.
- `prismrt.io.write_image(path, image, renderer)` (EXR linear, or 8-bit
  through the view).

### 6.2 Example: load a USD file and render to NumPy

```python
import numpy as np
import prismrt as prism

r = prism.Renderer()                    # RendererOptions.FromEnvironment() by default
scene = r.create_scene()
loaded = prism.usd.load(scene, "assets/scenes/CornellBox.usda", time=0)
scene.commit()                          # converts meshes, compiles materials (GIL released)

session = r.create_session(
    scene, device="cpu", width=512, height=512,
    camera=loaded.cameras["/World/Camera"],
    aovs=["color", "albedo", "normal"],
    settings=prism.RenderSettings(samples=64, adaptive_threshold=0.0, seed=1))
result = session.render()               # blocking, GIL released
rgb = np.asarray(result.aovs["color"])  # (512, 512, 3) float32, zero-copy view
print(result.mean_samples, session.device)
```

### 6.3 Example: a scene from NumPy arrays

```python
points = np.array([[-1, 0, -1], [1, 0, -1], [1, 0, 1], [-1, 0, 1]], np.float32)
indices = np.array([[0, 1, 2], [0, 2, 3]], np.int32)
uvs = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)

floor = scene.add_mesh(points=points, indices=indices, uvs=uvs,
                       uvs_interpolation="vertex")
gold = scene.add_material(mtlx_file="materials/gold.mtlx")
red = scene.add_material(diffuse=(0.8, 0.1, 0.1))
scene.add_instance(floor, transform=np.eye(4), materials=[red])

ball = scene.add_spheres(centers=np.zeros((1, 3), np.float32),
                         radii=np.array([0.5], np.float32))
# Motion shapes (section 5): accepted now, one instant rendered until 8b.
scene.add_instance(ball, materials=[gold],
                   transform=[(0.0, translate(0, 0.5, 0)), (1.0, translate(0.2, 0.5, 0))])

scene.add_light(shape="rect", width=2, height=2, intensity=10,
                transform=look_down(4))
scene.add_environment(color=(0.05, 0.05, 0.05))
cam = scene.add_camera(prism.Camera.look_at(eye=(0, 2, 5), target=(0, 0, 0),
                                            vertical_fov=40))
scene.commit()
```

### 6.4 Example: progressive render with a callback and cancellation

```python
import threading
stop = threading.Event()

def on_progress(p: prism.Progress) -> bool:
    print(f"\r{p.phase:10} {p.fraction:6.1%} {p.mean_samples:7.1f} spp", end="")
    return not stop.is_set()            # False cancels

session = r.create_session(scene, device="vulkan", width=1280, height=720,
                           camera=cam, settings=prism.RenderSettings(samples=1024))
session.start(on_progress)              # returns at once; passes run on a C++ thread
try:
    while session.wait(timeout=0.5) == prism.Status.RUNNING:
        preview = np.asarray(session.aov("color"))   # snapshot at a pass edge
except KeyboardInterrupt:
    session.cancel()
    session.wait()

# Blocking form: a False from the callback, or Ctrl-C, raises.
try:
    session.render(on_progress)
except prism.CancelledError:
    pass
```

### 6.5 Example: a pxr stage through `UsdUtils.StageCache`

```python
from pxr import Usd, UsdGeom, UsdUtils  # pxr first (DLL load order, 6.7)
import prismrt as prism

stage = Usd.Stage.Open("shot.usda")
UsdGeom.Sphere(stage.GetPrimAtPath("/World/Ball")).GetRadiusAttr().Set(2.0)  # unsaved edit

# What prism.usd.load(scene, stage) does inside:
stage_id = UsdUtils.StageCache.Get().Insert(stage).ToLongInt()
loaded = prism.usd._load_cached(scene, stage_id, time=Usd.TimeCode.Default())
#   C++: UsdUtilsStageCache::Get().Find(UsdStageCache::Id::FromLongInt(id))
scene.commit()
```
- Both sides must be the same USD build in the same process (same DLLs): the
  extension checks `pxr.Usd.GetVersion()` and the loaded `usd_usd` module
  against the version it was built with and raises `ImportError` otherwise.
- The stage stays in the cache while prism holds it; `load` removes the entry
  if it inserted it.
- Live updates (later): `prism.usd.StageSync(scene, stage)` built on
  `UsdImagingStageSceneIndex` and the same scene-index translator as the
  Hydra delegate (step 5), so stage edits become scene edits.

### 6.6 GIL, threads and zero-copy

GIL:
- Released (`nb::call_guard<nb::gil_scoped_release>`) in every call that can
  take long: `Renderer()` (MDL start-up), `commit`, `render`, `prepare`,
  `wait`, `aov` (readback/copy), `usd.load`, `gltf.load`, `obj.load`.
- Callbacks (progress, log) run on C++ threads. The binding takes the GIL
  (`nb::gil_scoped_acquire`), calls, and turns a Python exception into a
  cancel (rethrown later from the blocking call). The C++ side calls them
  without holding its own locks and at most ~10 times a second.
- Ctrl-C: a blocking call runs the work on a C++ thread and waits in 100 ms
  slices; between slices it takes the GIL and calls `PyErr_CheckSignals()`;
  on `KeyboardInterrupt` it cancels, joins and re-raises.
- `atexit`: cancel and join every session before finalization (3.8).
- Free-threaded Python (3.13t) is not a target for the first version.

Zero-copy inputs:
- Accepted: `nb::ndarray<const T, nb::c_contig, nb::device::cpu>` with the
  expected dtype and shape (`(N, 3)` float32 points, `(M, 3)` or flat int32
  indices, ...). The binding makes a `BufferView` whose `owner` holds a Python
  reference (its deleter takes the GIL).
- Other dtypes or layouts are converted once (a copy), not rejected.
- The core reads the buffers at `commit()` and converts them into its own
  structures (Embree buffers, GPU uploads); after `commit()` it drops the
  references. In-place changes to an array after `commit()` have no effect
  until `update_mesh_points` (or `update_mesh`) and another commit.
  Zero-copy into Embree (`rtcSetSharedGeometryBuffer`) is a later
  optimization (open question 8.8).

Zero-copy outputs:
- `Image` exposes `__array__` / the buffer protocol and `__dlpack__` /
  `__dlpack_device__` (`kDLCPU`), as a read-only `nb::ndarray` whose owner
  capsule keeps the C++ `Image` alive. `np.asarray(img)` and
  `torch.from_dlpack(img)` do not copy.
- Images are host memory. GPU-resident DLPack (`kDLVulkan`, `kDLMetal`)
  is out of scope (few consumers).

### 6.7 GPU devices from Python, and packaging constraints

Device selection:
- `prism.devices()` / `r.devices()` list what the build and machine have
  (`AvailableDevices`, CPU last).
- `create_session(..., device="auto" | "cpu" | "vulkan" | "metal",
  allow_fallback=True)`; `session.device` and `session.device_note` say what
  was used and why it fell back; `allow_fallback=False` raises
  `prism.UnsupportedError`.
- Default device for Python: open question 8.14 (GPU faults froze this PC).

What a Python package must carry (for the packaging task):
- `prism` shared library and its dependencies: Embree, oneTBB, MaterialX,
  OIIO, OCIO, OIDN (+ its device modules), the MDL SDK (`libmdl_sdk` and the
  `nv_openimageio` plugin), OpenSubdiv, vk_oidn.
- Data: the MaterialX libraries folder (default `materialx.libraries` =
  package-relative when `PRISM_MATERIALX_LIBRARY_PATH` is unset).
- GPU: the GPU devices compile HLSL at run time with the DXC executable
  (`hlsl_compiler.cpp:113-121`, `PRISM_DXC`): ship `dxc` + `dxcompiler` +
  `dxil`; Metal also needs the Metal shader converter library. Shaders are
  embedded already (`prism_embedded_shaders.cpp`).
- Windows DLL search: Python 3.8+ ignores `PATH` for dependencies;
  `__init__.py` calls `os.add_dll_directory` for the package's `bin`.
- Load order: `import pxr` before PySide6 (PySide6's old `msvcp140` breaks
  USD's DLLs); the same risk applies to prism's own MSVC-built DLLs.
  `__init__.py` can detect an older `msvcp140` already loaded and raise a
  clear error.
- `prismrt.usd` must match the user's `pxr` build exactly (6.5): a wheel per
  USD distribution or a bundled USD (open question 8.10).
- Writable cache directory (`RendererOptions::cacheDirectory`); the shader
  and texture caches must not land on the slow D: drive on this machine.

## 7. Migration plan

Rules for every step:
- The CLI and Hydra keep working; both build with `PRISM_BUILD_HYDRA=ON`
  (the parity harness needs it).
- Steps that should not change images are checked bit-identical against the
  previous build (`--exact`, CLAUDE.md), with `--adaptive-threshold 0` and
  fixed samples.
- Focused suites per step, the full serial `ctest -j 1` once at the end
  (merge gate). GPU tests only with the machine-safety rules.
- Load and edit timings compared with alternating runs of old/new builds
  (`tests/hydra/benchmark_loading.py`), reported as ratios.

| Step | What | Safety net | Size |
|---|---|---|---|
| 0 | João answers section 8; record decisions in `docs/DECISIONS.md` | — | S |
| 1a | Internal `Renderer::Impl` context + `RendererOptions::FromEnvironment()`; API-level env reads (rows 8, 11-18) go through it; front ends create the context | `prism_render_settings_test`, `prism_render_settings_hydra_test`, `prism_cli_*`, parity fixtures; `--exact` | S |
| 1b | Colour management explicit (row 7): loaders, materials, textures, lights, devices take it from the context/scene; delete `Active/SetActive/Activate` | `prism_color_management_test`, `prism_color_*` (patches, view, legacy CLI/Hydra), parity `color_*`, `gltf_uniform_colorspace`; `--exact` | M |
| 1c | SSS fallback and binding purposes as options (rows 9-10) | parity `sss_fallback`, `binding_*`, `prism_scene_features_sss_fallback_*`, `materialx_loader_sss` | S |
| 1d | Runtime object: caches and registries (rows 23-54) behind it; `TextureService` owned by the context; per-device `groupStackSize` (row 54); audit cache keys for colour identity and SSS fallback, with a regression test (same material, two working spaces, one process) | `prism_texture_service_test`, `prism_texture_streaming_test`, `prism_generated_mdl_*` (fresh, concurrent process), `prism_material_generation_cache_test`, `prism_standalone_material_reuse_test`, `prism_hydra_material_loading_test`, `prism_gpu_program_budget_test` (gpu); load benchmark | M |
| 1e | Progress and logging per renderer (rows 55-56, 58-59, 62); log sink callback | `prism_hydra_scene_bridge_test` (its progress-status cases, `tests/hydra/progress_status_test.cpp`), `prism_cli_clean_exit_*`, `prism_cli_time_limit`, `tests/cli_smoke.py` (manual) | S |
| 2 | Core without USD: `Matrix4d` in `camera_rays.h`/`aov.h`; std-only `MeshSource` split from `mesh_conversion`; render settings off `SceneManager` (2.7); CMake targets `prism`, `prism_usd`, `prism_gltf`, `prism_obj`, `prism_io`; a configure check that builds `prism` without pxr | `prism_camera_rays_test`, `prism_subdivision_test`, `prism_obj_*`, `prism_gltf_*`, parity (`subdivision_*`, `displacement`, `camera_exposure`, `id_aovs`); `--exact` (camera math must be bit-identical) | M |
| 3 | Public `Scene` API (handles; `commit()` = full rebuild); glTF, OBJ and USD loaders emit into it; the CLI uses it | all parity fixtures (the Hydra side is unchanged, so a CLI regression shows as a mismatch: `instances`, `instance_primvars`, `linking`, `lights`, `env_dome`, `implicit_surfaces`, `animated_*`, `skinned_*`, `id_aovs`, `double_sided`), `prism_gltf_*`, `prism_obj_*`, `prism_usd_spheres_test`, `prism_scene_features_*_cli`; `--exact` vs the previous CLI | L |
| 4 | Public `Session` batch path (`render()`, `RenderResult`, AOVs, exposure, denoise) and `prism_io`; the CLI becomes a thin front end | `prism_render_products_*`, `prism_render_crop_*`, `prism_denoise_*` (CPU; Vulkan with the gpu label), `prism_scene_features_light_aovs_*`, `prism_cli_time_limit`, CPU/Vulkan parity tests (`*_vulkan_parity`); `--exact` | M |
| 5a | Progressive `Session` (`start/pause/cancel/restart/aov/pick`): move the pass loop, previews, adaptive publication, denoise interval and GPU preparation preview out of `scene_bridge.cpp`; Hydra still builds its own `SceneManager` | `prism_hydra_scene_bridge_test`, `prism_hydra_adaptive_publication_*`, `prism_hydra_render_buffer_test` (the bridge test includes the picking, accumulation and multisampled-presentation cases), `prism_hydra_time_limit`, `prism_denoise_interval_hydra`, parity | L |
| 5b | Incremental `commit()`: dirty kinds, double-buffered revisions (the snapshot/bank machinery moves into the core); Hydra's scene-index observer becomes a translator to `Scene` edits (placements, materials, lights first; structure and prototypes last) | `prism_hydra_setting_change_test`, `prism_hydra_animation_exercise`, `prism_hydra_scene_bridge_test` (dependency-update and environment-cache cases), streaming tests, parity; edit-latency benchmark; manual usdview check | L |
| 6 | `prism` as a shared library with `PRISM_API` exports, internals hidden; `hdPrism`, CLI link it | full serial `ctest`; usdview smoke; export list review | M |
| 7 | Python bindings (nanobind) + pytest under CTest (CPU; Vulkan tests with the gpu label and lock) | new tests: Python-vs-CLI images exact (CPU, fixed seed), zero-copy (array addresses), cancellation, StageCache, `atexit` join | M |
| 8 | Packaging (wheel, DLLs, load order) | install test in a clean venv | M-L |
| 9 | Motion blur (Phase 8b) on the reserved shapes | its own plan | L |

Notes:
- Step 6 is mechanical and can move earlier if the CLI/Hydra double copy of
  globals bites (it does not today: no process loads both).
- Steps 1a-1e keep today's internal APIs and only make state explicit, so
  they are low risk and give most of the "no globals" value early.
- Step 5 is the largest and riskiest; split as above and keep the old
  `SceneBridge` path working until 5b lands.

Risks:
- Hydra edit latency and threading (5b): the snapshot/bank code is tuned
  (spare banks, prototype pools, residency reuse); moving it can regress
  usdview interactivity. Mitigation: move code, not rewrite; benchmarks.
- Cache keys (1d): today they can assume one colour space and one SSS setting
  per process; with several renderers a missing key component returns a wrong
  material silently. Mitigation: the regression test in 1d.
- MDL SDK single instance: the Runtime must be the only owner; a second copy
  of `prism` in the process (static linking) breaks it. Mitigation: step 6
  before Python ships.
- Parity tests exist only with `PRISM_BUILD_HYDRA=ON`: keep Hydra in every
  build during the migration.
- USD ABI coupling for Python (6.5): mismatched `pxr` builds crash, not
  error. Mitigation: version check at import.
- Exactness: moving math off `GfMatrix4d` (step 2) can change rounding;
  `--exact` comparisons catch it, and a noise-floor comparison decides if a
  change is acceptable.

## 8. Open questions for João

1. **Errors**: C++ exceptions (`prism::Error` with codes; matches the code
   today, maps to Python exceptions) or status codes? Proposal: exceptions.
2. **C API**: none until the first release (proposal), or a C API now (as
   Embree, OIDN, OSPRay) for ABI stability and other languages?
3. **Names**: `prism::Renderer` / `Scene` / `Session`? Python package name
   (`prismrt`; `prism` is likely taken on PyPI) and import name.
4. **Shared library**: `prism` as a DLL (needed once `hdPrism` and Python
   share a process). Keep a static option for the CLI?
5. **Several renderers per process** with different colour spaces / SSS
   settings (proposal: yes, with audited cache keys), or one active renderer
   with a clear error?
6. **Environment variables**: should Python honour `PRISM_*` by default
   (proposal: yes, `Renderer(env=False)` to opt out)? Which env vars become
   documented options and which stay developer-only (rows 19, 63)?
7. **Scene threading**: single writer per scene + `commit()` (proposal), or
   internally synchronized edits from many threads (costlier)?
8. **Mesh data**: copy at `commit()` (proposal for v1) or zero-copy shared
   buffers into Embree (lower memory; the caller must keep arrays unchanged)?
9. **MaterialX in the API**: files and strings only, plus an optional
   header-only interop for `MaterialX::DocumentPtr` (proposal), or MaterialX
   types in the public API?
10. **Packaging targets**: Windows first? Linux, macOS? Which USD for
    `prismrt.usd` (our bundled build, `usd-core` from PyPI, Houdini/Omniverse
    builds)? Which Python versions (3.11 only today)?
11. **Versioning**: 0.x semver with `PRISM_VERSION_*` macros and
    `prismrt.__version__`; when is the first release that freezes the API?
12. **Hydra**: one `Renderer` per delegate instance (proposal: isolation,
    heavy caches still shared through the Runtime) or one per process?
13. **Cancellation**: progress callback returning `False` plus an explicit
    `cancel()` (proposal: both)? Default callback rate (10 Hz)?
14. **Python default device**: `auto` like the CLI, or `cpu` unless asked,
    given the GPU faults on this machine?
15. **Order**: as planned (core + CLI, then Hydra, then Python), or Python
    bindings on the batch API right after step 4 (earlier use; Hydra port in
    parallel)?
16. **AOV names**: Hydra tokens (`color`, `primvars:st`, `primId`) as the
    public string names next to the enum?
17. **Redistribution**: are the MDL SDK, DXC, Metal shader converter and
    vk_oidn licences fine for wheels (for the packaging task)?
