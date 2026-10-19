# Source this script (bash or zsh) before running PrismRT or usdview from a
# configured macOS/Linux build. Optional per-machine overrides belong in the
# ignored prism_local_env.sh or the PRISM_* environment variables documented
# in README.md.
if [ -n "${BASH_SOURCE:-}" ]; then _prism_script="${BASH_SOURCE[0]}"; else _prism_script="${(%):-%x}"; fi
_prism_root="$(cd "$(dirname "$_prism_script")" && pwd)"
[ -f "$_prism_root/prism_local_env.sh" ] && . "$_prism_root/prism_local_env.sh"

# Runtime roots recorded at configure time (prism-runtime.json).
_prism_config="$_prism_root/prism-runtime.json"
for _candidate in "$_prism_root/build-mac/prism-runtime.json" "$_prism_root/build/prism-runtime.json"; do
    [ -f "$_prism_config" ] || _prism_config="$_candidate"
done
_prism_json() {
    if command -v plutil >/dev/null 2>&1; then
        plutil -extract "$1" raw -o - "$_prism_config" 2>/dev/null
    else
        python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2],''))" "$_prism_config" "$1" 2>/dev/null
    fi
}
if [ -f "$_prism_config" ]; then
    : "${PRISM_USD_ROOT:=$(_prism_json usdRoot)}"
    : "${PRISM_MATERIALX_ROOT:=$(_prism_json materialXRoot)}"
    : "${PRISM_MATERIALX_LIBRARY_PATH:=$(_prism_json materialXLibraries)}"
    : "${PRISM_MATERIALX_MDL:=$(_prism_json materialXMdl)}"
    : "${PRISM_MDL_SDK_ROOT:=$(_prism_json mdlSdkRoot)}"
    : "${PRISM_PYTHON_ROOT:=$(_prism_json pythonRoot)}"
    export PRISM_USD_ROOT PRISM_MATERIALX_ROOT PRISM_MATERIALX_LIBRARY_PATH PRISM_MATERIALX_MDL
    export PRISM_MDL_SDK_ROOT PRISM_PYTHON_ROOT
fi

# Command Line Tools work without an accepted Xcode license.
if [ "$(uname)" = "Darwin" ] && [ -z "${DEVELOPER_DIR:-}" ] &&
   [ -d /Library/Developer/CommandLineTools ]; then
    export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

_prism_prepend() {  # _prism_prepend VAR dir...
    local var="$1"; shift
    local dir
    for dir in "$@"; do
        [ -n "$dir" ] && [ -d "$dir" ] || continue
        eval "local current=\"\${$var:-}\""
        case ":$current:" in *":$dir:"*) ;; *) eval "export $var=\"$dir\${current:+:\$current}\"" ;; esac
    done
}

_prism_libvar=LD_LIBRARY_PATH
[ "$(uname)" = "Darwin" ] && _prism_libvar=DYLD_LIBRARY_PATH
_prism_prepend "$_prism_libvar" \
    "${PRISM_USD_ROOT:+$PRISM_USD_ROOT/lib}" \
    "${PRISM_MATERIALX_ROOT:+$PRISM_MATERIALX_ROOT/lib}" \
    "${PRISM_MDL_SDK_ROOT:+$PRISM_MDL_SDK_ROOT/lib}" \
    "$_prism_root/build-mac/hydra/plugin"
_prism_prepend PATH \
    "${PRISM_USD_ROOT:+$PRISM_USD_ROOT/bin}" \
    "${PRISM_PYTHON_ROOT}" \
    "$_prism_root/build-mac"
_prism_prepend PYTHONPATH \
    "${PRISM_USD_ROOT:+$PRISM_USD_ROOT/lib/python}" \
    "$_prism_root/tools/usdview_plugins"

if [ -d "$_prism_root/build-mac/hydra/resources" ]; then
    _prism_prepend PXR_PLUGINPATH_NAME "$_prism_root/build-mac/hydra/resources"
fi
_prism_installed="$(dirname "$_prism_root")/lib/usd/plugins/prism"
if [ -f "$_prism_installed/resources/plugInfo.json" ]; then
    _prism_prepend PXR_PLUGINPATH_NAME "$_prism_installed/resources" \
        "$_prism_installed/usdview_plugins/prismUsdview"
    _prism_prepend PYTHONPATH "$_prism_installed/usdview_plugins"
    _prism_prepend "$_prism_libvar" "$_prism_installed/plugin"
elif [ -f "$_prism_root/tools/usdview_plugins/prismUsdview/plugInfo.json" ]; then
    # usdview discovers the extension through its plugInfo.json, not PYTHONPATH.
    _prism_prepend PXR_PLUGINPATH_NAME "$_prism_root/tools/usdview_plugins/prismUsdview"
fi

if [ -n "${PRISM_MATERIALX_LIBRARY_PATH:-}" ] && [ -z "${PRISM_MATERIALX_MDL:-}" ]; then
    export PRISM_MATERIALX_MDL="$PRISM_MATERIALX_LIBRARY_PATH/mdl"
fi
if [ -n "${PRISM_MDL_SDK_ROOT:-}" ]; then
    export MDL_SDK_ROOT="$PRISM_MDL_SDK_ROOT"
    [ -n "${PRISM_MATERIALX_MDL:-}" ] && export PRISM_MATERIALX_MDL_LIBRARIES="$PRISM_MATERIALX_MDL"
fi
if [ "$(uname)" = "Darwin" ]; then
    : "${PRISM_CACHE_DIR:=$HOME/Library/Caches/PrismRT}"
    export PRISM_CACHE_DIR
fi
: "${PYOPENGL_USE_ACCELERATE:=0}"
export PYOPENGL_USE_ACCELERATE
unset _prism_script _prism_root _prism_config _prism_candidate _prism_installed _prism_libvar
unset -f _prism_json _prism_prepend
