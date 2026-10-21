# Texture service

The texture service writes immutable float, tiled and mipmapped OpenEXR files
(`<content key>.tx.exr`) and GPU-ready copies (`<content key>.gpu`) to a
content-addressed disk cache. They used to be
named `.tx`, which OIIO opens by trying its TIFF reader and then every other
format; older `.tx` entries are still trimmed. By default, the cache is `%LOCALAPPDATA%/PrismRT/cache` on Windows,
with the system temporary directory as the fallback when `LOCALAPPDATA` is
unavailable. Set `PRISM_CACHE_DIR` to choose another directory.

## Preparation pipeline

A cold preparation decodes the source once, in its own storage (8-bit,
16-bit, half or float; OIIO converts to float exactly as a float decode
would), and produces two files from that decode:

- `<key>.gpu`, the GPU-ready copy (`src/texture_gpu_file.h`): the top level at
  the source's precision with only the texture's own channels (RGB is not
  padded), after a 256-byte header with a level table and a digest of the
  payload. 16-bit texels that are all multiples of 257 are stored as 8-bit;
  a transformed image (unassociated alpha, gamma, exposure, channel or window
  changes) is stored as float with the statistics a device needs to choose
  8-bit, half or float storage. It is written to a temporary file, flushed
  and renamed into place; every read checks size and digest, and a damaged
  copy is written again from the source.
- `<key>.tx.exr`, the CPU-side texture: the same float texels and box-filtered
  mip chain OIIO's `make_texture` produced (bit-identical, checked by
  `prism_texture_writer_test`), written by `src/texture_writer.h` with
  `zip:1` compression (`PRISM_TEXTURE_COMPRESSION`; about 3x faster than zip's
  default level with eight concurrent writers, 5% larger) and mip levels
  computed in parallel.

Preparation returns once the GPU copy is published; background writers
(`PRISM_TEXTURE_WRITER_THREADS`, default the hardware threads) build the
mips and write the `.tx.exr`. Lookups, `readPixels` and `preparedPath()` wait
for it, so the CPU device and the Metal path see the same textures, while
the Vulkan device uploads from the GPU copies without waiting. A second
preparation of content still being written waits on that writer. The service
finishes queued writes before it is destroyed; a process that is killed
leaves temporary files, which a later trim removes once their process has
exited. `PRISM_TEXTURE_BACKGROUND_WRITES=0` writes the `.tx.exr` before
preparation returns. Cold preparations wait for a memory budget
(`PRISM_TEXTURE_PREPARE_MB`, default an eighth of physical memory between 2
and 16 GiB) that a queued write keeps until it finishes; the tiles of a UDIM
texture are prepared in parallel within it.

The writer exists because of a hang: OpenEXR's `TiledOutputFile::writeTiles`
waits on a tile buffer's semaphore, writes the compressed tile and posts the
semaphore. When that write throws (an I/O error, or an allocation failure
under a memory cap), the semaphore is never posted; OIIO catches the
exception, and its `write_image` goes on to the next row of tiles, whose
first task waits on the leaked buffer forever, with every IlmThread worker
idle. It is not a thread-pool deadlock: it happens with OpenEXR threading off
too. `texture_writer` writes through a stream that records a failure instead
of reporting it to OpenEXR, checks each row, and stops at the first failure;
in-memory sources are materialized through the same stream.

Warm loads trust a source's recorded digest instead of reading and hashing
it again (`src/fingerprint_records.h`): records in `<cache>/fingerprints`
are keyed by the file's path, size, modification time, resolver identity
and source version, plus its change time and file ID, which every write
updates even when the modification time is restored. A record also names
the published artifact it vouches for with that artifact's stamp, so a
replaced artifact is verified again. Materialized resources, UDIM tile
publications and directly prepared sources use them.

GPU copies are format version 2: a 1 KiB header with a level table and one
digest per level, and the whole mip chain (`BuildMipChain`: each level the
2 x 2 box of the previous one for even sizes, the bilinear center tap of
`vkCmdBlitImage` for odd ones, rounded to the stored precision; checked
against a reference box filter on every OpenPBR and Teapot copy). A device
reads any range of levels (`ReadLevels`) and checks only their digests.
Version 1 copies fail validation and are written again from the source on
first use.

The stored chain is the single reference for every Vulkan image: whole
images (streaming off, reduced to fit the budget, early uploads, domes) and
streamed ones upload its levels, so an image's texels never depend on
which levels are resident. Mips generated with blits depend on the level
they start from (on OpenPBR they differ enough to move the mean by 0.65%);
`PRISM_GPU_TEXTURE_BLIT_MIPS=1` restores them for diagnostics. The CPU
device samples the `.tx.exr` mips OIIO filters in float; taking the GPU
levels from those files (instead of a box chain at the source's precision)
is not done yet.

## GPU texture streaming

The Vulkan device keeps only the mip levels shading asks for
(`src/device/gpu/texture_streaming.h`; `PRISM_GPU_TEXTURE_STREAMING=0` or
`prism:textureStreaming false` turn it off and upload whole images, reduced
to fit the budget, as before):

- **Residency.** Each streamed image (every image with a version 2 GPU copy,
  except domes) holds a contiguous range of levels, from its finest resident
  level to 1 x 1. Its `VkImage` is allocated at the finest resident level's
  size, so device memory holds only those levels, and sampling with
  normalized coordinates clamps the level of detail to them without a clamp
  operand. `GpuImage.info.w` is the finest level, for texel loads
  (`gpu_image_texel`), and `info.xy` the full size (resolution queries and
  crops match the CPU). At scene setup only the tails (levels at most
  `PRISM_GPU_STREAMING_TAIL`, default 128, texels on a side) are read.
- **Feedback.** One pixel of each 4 x 4 block (`PRISM_GPU_STREAMING_FEEDBACK_STRIDE`,
  moving every pass; every pixel while settling) adds, for each lookup, the
  path's weight to a per-image histogram of the levels asked for. The level
  is Vulkan's anisotropic LOD rule on the ray-footprint gradients, half a
  level finer (`PRISM_GPU_STREAMING_LOD_MARGIN`) for implementations'
  approximations; texel loads ask for level 0. The weight is the path
  throughput's largest component, clamped to 1 (`pathWeight`, set before each
  material call). The same submission as the dispatch copies the histograms
  into one of three host buffers and clears them; the streaming thread reads
  them (no stall: without a free buffer they stay for the next copy). Metal
  compiles none of it (`PRISM_TEXTURE_STREAMING` is defined only by the
  Vulkan resources).
- **Requests.** An image's request is the coarsest level that serves all but
  `PRISM_GPU_STREAMING_THRESHOLD` (default 0.001) of its lookup weight in the
  current view; per image, and therefore in total. Levels only negligible
  paths ask for are left out: a bounded bias in filter footprint (those
  lookups read a blurrier level), never in radiance. Without the weighting,
  the finest level any path ever asks for keeps getting finer as samples
  accumulate (OpenPBR: still changing after 8 feedback rounds); with it,
  requests settle in 2-3 rounds.
- **Planning** (deterministic): tails always; levels resident and requested
  in the current view are never demoted; missing requested levels one at a
  time by lookup weight per byte within the budget
  (`PRISM_GPU_TEXTURE_BUDGET_MIB` less the whole images); then other
  resident levels while they fit, least recently requested evicted first.
  Requests the budget cannot hold are reported once per view:
  `N of M streamed textures below the requested resolution`.
- **Uploads.** A streaming thread creates each changed image anew from the
  copy's levels (read straight into host-cached staging, batches of
  `PRISM_GPU_STREAMING_BATCH_MIB`, own command pool and fence); the render
  thread swaps descriptors and records before its next dispatch, when none
  of its submissions is pending, and frees the old image.
- **Offline renders** (`prism:textureStreamingPrepass`, which
  `prism_hydra_render` sets, and the standalone renderer) trace the first
  samples of the accumulation (same mode, seeds and random numbers) as
  feedback-only rounds until a round changes nothing (at most
  `PRISM_GPU_STREAMING_SETTLE_ROUNDS`, 8), then freeze residency and stop
  recording feedback for the whole accumulation: the freeze makes final
  images deterministic (repeated runs are bit-identical).
- **Interactive rendering** restarts an accumulation at most once, before
  `PRISM_GPU_STREAMING_RESTART_SAMPLES` (16) samples, once the levels the
  first feedback asked for are resident; a new view or scene starts a new
  accumulation as usual. Later residency changes apply in place: the image
  sharpens, and the few samples taken with coarser levels wash out.
- **Identity.** With every level resident (`PRISM_GPU_STREAMING_TAIL=65536`)
  the streamed image is bit-identical to whole images. Leaving levels out
  can only differ where a lookup asked for a level the feedback did not
  record: on OpenPBR at 960 x 540, 64 spp, with exact requests (threshold 0),
  39 of 518,400 pixels differ (NRMSE 0.0012). Feedback over all 64 samples
  leaves 4, and a 1.5-level margin as well leaves none (the hardware picks a
  level up to 1.5 finer than the specification's rule for a few lookups);
  there is no level-shift error. The default margin of 0.5 keeps memory
  lower (4.1 GB against 6.5 GB there).
- Statistics: `prismTextureStreaming`, `prismTextureResidentMiB`,
  `prismTextureBudgetMiB`, `prismTextureMemoryMiB`, `prismDeviceMemoryMiB`,
  `prismTexturesPending`, `prismTexturesBelowRequested`,
  `textureStreamingRestarts`; the usdview HUD shows
  `Texture: resident / budget MiB` (with pending and below-requested counts).
- **Descriptors and allocations.** Every image, streamed or not, is still one
  descriptor (`kMaxImages`, 4096) and one `vkAllocateMemory` (NVIDIA allows
  4096): streaming does not lift ALab's limits. Tiles would add a page table
  per image and sparse or suballocated memory; the per-level offsets of the
  GPU copy and the planner's budget and priority rules carry over.

Measurements: [performance.md](performance.md#gpu-texture-streaming-october-2026).

## Cache configuration

`PRISM_TEXTURE_CACHE_MB` sets OIIO's shared texture tile-cache budget in MiB
(default a sixteenth of physical memory, between 512 and 4096; 512 MiB made
ALab render about 21 times slower than 4 GiB). `PRISM_DISK_CACHE_GB` sets the disk cache's LRU budget in GiB
(default 20; GPU copies add about half again to the float `.tx.exr` files,
and a budget below a scene's working set evicts, at the start of every warm
load, files that load prepares again). Disk trimming counts `.tx.exr` and
`.gpu` files and skips those pinned by live resource handles in any
process. Source files are not modified.

`PRISM_TEXTURE_OPEN_FILES` sets how many prepared files the TextureSystem keeps
open. The default is the process's file limit less 512, at most 7680 (on
Windows the C runtime's stdio limit is raised to 8192 first). OIIO's own default
of 100 makes a scene with thousands of textures, such as ALab, close and reopen
files on almost every lookup.

The persistent disk allowance is partitioned between prepared textures and
generated MaterialX source templates. Generated templates receive the smaller
of 256 MiB and one sixteenth of `PRISM_DISK_CACHE_GB`; prepared textures receive
the remainder. A template larger than its allowance is generated normally and
is not persisted. Both pools evict closed artifacts by recency. Resolver-
materialized source files are not yet covered by this allowance.

Eligible color-only MaterialX variants persist a checksummed generated-MDL
template below `generated-mdl` in the same cache root. Its content key includes
the normalized graph, material name, MaterialX version, all generator options,
and the complete contents of the configured MaterialX library tree. Documents
with external XInclude implementation sources conservatively bypass template
reuse. Cache hits still substitute each current authored color and validate the
recorded source spans; native MDL code and SDK objects remain process-local.
Set `PRISM_DISABLE_GENERATION_CACHE=1` to bypass both memory and persistent
template reuse. With `PRISM_MDL_VERBOSE=1`, generated-source diagnostics are
written below `generated-mdl/debug` instead of beside source assets.

Preparation options participate in cache identity. `gamma` is the exponent
applied to color channels before filtering; `exposure` multiplies them by
`2^exposure`. The selected alpha channel is excluded from both operations.
`alphaChannel = -1` uses source metadata (with gray-alpha inference), `-2`
disables alpha, and nonnegative values select a source channel. `resolverIdentity`
partitions otherwise identical content prepared through different resolver
contexts. `colorSpace` also partitions identity and is attached as source
interpretation metadata during preparation; OIIO's `.tx` writer may not retain
that attribute in the file header. It does not itself convert pixel values.
`unassociateAlpha` defaults to `false` to preserve existing callers. When it is
`true`, OIIO is asked to preserve unassociated input during decode; associated
RGB is divided by alpha before gamma, exposure, and mip generation. Explicit
`oiio:UnassociatedAlpha` metadata is honored, and PNG input follows its
straight-alpha storage convention so its RGB is not divided a second time.
Alpha is copied unchanged. Associated RGB with exactly zero alpha becomes
finite black in the prepared texture; the MDL SDK canvas currently yields
nonfinite RGB for that same singular input, so parity is checked strictly for
positive alpha and the service intentionally keeps the finite result at zero.

`TextureService::invalidate(path, options)` removes the prepared file and GPU copy for the
source's current content fingerprint when no process holds it. A changed source
file naturally hashes to a different cache entry. The in-memory texture
preparation overload hashes pixel values and image specification, so generated
or embedded images use the same immutable cache pipeline.

The scalability fixture exercises 20 tiled 512 × 512 RGBA float textures with
an 80 MiB source-pixel total against a 32 MiB tile-cache budget. Its reported
filtered-versus-bilinear RMS and maximum are differences between two sampling
modes; they are not accuracy errors against ground truth. Cold and warm runs
refer to prepared `.tx` cache state and do not clear the operating system file
cache. For a short runtime/relocation check, run
`python tests/benchmark_scalability.py --executable <installed-helper.exe>
--output <fresh-output-directory> --samples 1024 --parity-only`. It compares
both filtered and bilinear checksums across two fresh helper processes with the
same fixture, sample count, and cache settings. `tools/validate_install.ps1`
copies this test helper beside the relocated launcher and runs that check with
build-tree runtime paths removed from `PATH`.

## Native MDL resource arguments

Class-compiled native MDL targets share machine code while each material owns
an immutable argument block and texture registry. Argument blocks created for
an existing target are copied out of the SDK interface before the material
transaction closes. Unsupported resources, including texture selectors, reject
that reuse path and regenerate the complete target with the SDK resource
handler; custom and SDK handlers are never mixed in one target.

`prism_mdl_texture_target_test` executes the generated BSDF, rather than only
calling the texture callback directly. It covers an ordinary 2D texture, two
UDIM tiles, different textures bound to the same cached class program, changed
string arguments, and the whole-target selector fallback. A non-null resource
or nonempty string that the cached target cannot represent sets the callback
failure flag, rejects the cached argument block, and regenerates the target.
The successful 2D and UDIM cache-hit cases therefore also verify that no valid
test resource returned the invalid index.

All retained MDL interfaces obtained while registering a texture are owned by
`mi::base::Handle`. In particular, the texture type returned by `get_type()`
must be released before its transaction closes. The native target regression
checks repeated cache-hit and UDIM transactions and exits without the SDK
`API:DATABASE` tag-reference diagnostic on MDL SDK 2025.0.3.

The target test closes each compilation transaction before executing the target,
matching production ownership. It checks selector fallback colors and resource
isolation, and sends nonzero UV derivatives through real JIT code to a
high-frequency checker texture. The filtered diffuse result must match the
checker mean and differ from the zero-derivative sample.

Arbitrary MDL scene-data/primvar queries are not implemented: their callbacks
return the material-provided defaults. Texture support does not imply support
for arbitrary shader primvars.
