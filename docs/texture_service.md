# Texture service cache configuration

The texture service writes immutable float, tiled and mipmapped OpenEXR files
(`<content key>.tx.exr`) to a content-addressed disk cache. They used to be
named `.tx`, which OIIO opens by trying its TIFF reader and then every other
format; older `.tx` entries are still trimmed. By default, the cache is `%LOCALAPPDATA%/PrismRT/cache` on Windows,
with the system temporary directory as the fallback when `LOCALAPPDATA` is
unavailable. Set `PRISM_CACHE_DIR` to choose another directory.

`PRISM_TEXTURE_CACHE_MB` sets OIIO's shared texture tile-cache budget in MiB
(default 512). `PRISM_DISK_CACHE_GB` sets the disk cache's LRU budget in GiB
(default 10). Disk trimming skips prepared textures pinned by live resource
handles in any process. Source files are not modified.

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

`TextureService::invalidate(path, options)` removes the prepared file for the
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
