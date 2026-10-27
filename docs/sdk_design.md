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
| 7 | Active colour management | `gActive` `src/color/color_management.cpp:273-300` (`Active`, `SetActive`, `Activate`). Set: `standalone_cli.cpp:855`, `scene_bridge.cpp:5219,5376`. Read at load: `lights.h:176,179,196`, `environment.h:245`, `texture.h:62,122,242`, `texture_service.cpp:654`, `mdl_material.h:98`, `mdl_generator.cpp:272`, `prepared_template.h:402,438`, `gltf_mesh.h:135,433`, `scene_bridge.cpp:2486`. Read **while rendering**: `cpu_device.cpp:31`, `gpu_device.cpp:72`, `light_table.h:101`, `environment.h:79` | Process-wide, "last Activate wins"; Hydra rebuilds the scene on change | **Renderer** (immutable `std::shared_ptr<const ColorManagement>`). Scenes keep the renderer's pointer; render-time reads take it from the scene, not a global. Changing colour = `Renderer::setColorManagement` which marks every scene for reload (Hydra's behaviour today). |
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
