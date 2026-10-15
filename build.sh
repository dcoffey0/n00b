# debugoptimized, not debug. -O0 leaves every `static inline` as a real call,
# and ncc wraps each libn00b function in a gc-stack-map push/pop pair whose
# body re-runs the TLS-free n00b_thread_self() recovery. Profiling
# test_regex_resharp at -O0 put 53% of total runtime in those two functions;
# the same corpus takes 78s at -O0 and 19s at -O2 on the same machine.
#
# Nothing is given up for it. The ncc flag set is identical at both levels,
# -DN00B_DEBUG is still set, assertions stay on (debugoptimized does not
# define NDEBUG), and -O2 ends up with MORE gc-frame call sites than -O0
# because inlining duplicates framed functions.
#
# Set N00B_BUILD_TYPE=debug for a stepping-friendly build.
N00B_BUILD_TYPE=${N00B_BUILD_TYPE:-debugoptimized}
N00B_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
N00B_CLEAN=${N00B_CLEAN:-0}
N00B_BUILD_TARGETS=${N00B_BUILD_TARGETS:-}
N00B_TEST=${N00B_TEST:-0}
N00B_TEST_FAIL_FAST=${N00B_TEST_FAIL_FAST:-0}
N00B_TEST_SUITES=${N00B_TEST_SUITES:-}
N00B_TEST_NO_SUITES=${N00B_TEST_NO_SUITES:-}
N00B_TESTS=${N00B_TESTS:-}
N00B_TEST_ALL=${N00B_TEST_ALL:-0}
N00B_DOCS=${N00B_DOCS:-0}
N00B_CROSS=${N00B_CROSS:-}
N00B_JOBS=${N00B_JOBS:-}
N00B_NATIVE=${N00B_NATIVE:-0}
N00B_ROCS_TRACE=${N00B_ROCS_TRACE:-0}
N00B_ROCS_TRACE_EVERY=${N00B_ROCS_TRACE_EVERY:-256}
N00B_UNICODE_ALLOW_DOWNLOADS=${N00B_UNICODE_ALLOW_DOWNLOADS:-1}
N00B_UNICODE_STRICT_CACHE=${N00B_UNICODE_STRICT_CACHE:-1}
N00B_UNICODE_CACHE_DIR=${N00B_UNICODE_CACHE_DIR:-}
N00B_BUILD_ARGS=()

function fail {
    echo "ERROR: $*" >&2
    exit 1
}

function usage {
    cat <<'EOF'
Usage: bash build.sh [options] [build_dir]

Options:
  --test               Build and run the default test set.
  --all-tests          Build and run all tests, including tests tagged long.
  --build-tests        Build the ~470 test executables as part of the default
                       compile (off by default; `--test` builds+runs on demand).
  --no-build-tests     Force-disable building tests in the compile step.
  --ccache             Use ccache as a compiler launcher in front of ncc.
  --no-ccache          Force-disable ccache.
  --fast-linker=WHICH  Linker selection: auto (default), off, mold, or lld.
  --no-fast-linker     Shorthand for --fast-linker=off.
  --help               Show this help.

Flags override the corresponding environment variable below.

Environment:
  N00B_TEST=1          Run tests after building. Long tests are skipped by default.
  N00B_BUILD_TESTS=1   Build the test suite in the compile step (see --build-tests).
  N00B_CCACHE=1        Use ccache in front of ncc (see --ccache).
  N00B_FAST_LINKER=... auto|off|mold|lld (see --fast-linker).
  N00B_TEST_ALL=1      Include tests tagged long.
  N00B_TESTS="..."     Pass explicit Meson test names; targeted tests are not filtered.
  N00B_TEST_SUITES     Pass explicit Meson suites.
  N00B_TEST_NO_SUITES  Pass explicit Meson suites to skip.
  N00B_ROCS_TRACE=1    Compile ROCS ingest/shard memory instrumentation.
  N00B_ROCS_TRACE_EVERY=N
                        Trace every N committed ROCS records when enabled.
                        Use 0 for seal-only tracing. Default: 256.
  N00B_UNICODE_ALLOW_DOWNLOADS=0|1
                        Permit first-build Unicode data downloads. Default: 1.
  N00B_UNICODE_STRICT_CACHE=0|1
                        Fail when Unicode conformance test data is missing. Default: 1.
                        Required table cache files are always fatal.
  N00B_UNICODE_CACHE_DIR
                        Optional Unicode cache directory. Relative paths are source-root relative.
EOF
}

function meson_bool {
    local value=$1
    local name=$2

    case "${value}" in
        1|true|TRUE|yes|YES|on|ON)
            echo true
            ;;
        0|false|FALSE|no|NO|off|OFF)
            echo false
            ;;
        *)
            fail "invalid boolean for ${name}: ${value}"
            ;;
    esac
}

function parse_args {
    while [[ $# -gt 0 ]] ; do
        case "$1" in
            --test)
                N00B_TEST=1
                ;;
            --all-tests)
                N00B_TEST=1
                N00B_TEST_ALL=1
                ;;
            --build-tests)
                N00B_BUILD_TESTS=1
                ;;
            --no-build-tests)
                N00B_BUILD_TESTS=0
                ;;
            --ccache)
                N00B_CCACHE=1
                ;;
            --no-ccache)
                N00B_CCACHE=0
                ;;
            --fast-linker=*)
                N00B_FAST_LINKER="${1#*=}"
                ;;
            --fast-linker)
                shift
                [[ $# -gt 0 ]] || fail "--fast-linker requires a value (auto|off|mold|lld)"
                N00B_FAST_LINKER="$1"
                ;;
            --no-fast-linker)
                N00B_FAST_LINKER=off
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            --)
                shift
                N00B_BUILD_ARGS+=("$@")
                break
                ;;
            --*)
                fail "unknown option: $1"
                ;;
            *)
                N00B_BUILD_ARGS+=("$1")
                ;;
        esac
        shift
    done
}

function add_test_no_suite {
    local suite=$1
    local current
    for current in ${N00B_TEST_NO_SUITES}; do
        if [[ "${current}" == "${suite}" ]] ; then
            return 0
        fi
    done
    if [[ -n "${N00B_TEST_NO_SUITES}" ]] ; then
        N00B_TEST_NO_SUITES="${N00B_TEST_NO_SUITES} ${suite}"
    else
        N00B_TEST_NO_SUITES="${suite}"
    fi
}

function test_suite_requested {
    local suite=$1
    local current
    for current in ${N00B_TEST_SUITES}; do
        if [[ "${current}" == "${suite}" ]] ; then
            return 0
        fi
    done
    return 1
}

parse_args "$@"

function select_bootstrap_compiler {
    if [[ -n "${CC:-}" ]] ; then
        export CC
        return
    fi

    if command -v clang &>/dev/null ; then
        export CC=$(command -v clang)
        return
    fi

    if command -v cc &>/dev/null && cc --version 2>/dev/null | head -n 1 | grep -qi 'clang' ; then
        export CC=$(command -v cc)
        echo "Using cc because it resolves to a clang-compatible compiler."
        return
    fi

    echo "No clang-compatible bootstrap compiler found." >&2
    echo "Install clang or rerun with CC=/path/to/clang-compatible-compiler." >&2
    exit 1
}

select_bootstrap_compiler

# n00b sources MUST be compiled with ncc. clang links but the resulting
# binary is wrong at runtime (it lacks the gc-stack-maps / auto-gc-roots
# transforms), and clang also silently accepts n00b-dialect mismatches
# that ncc rejects. Resolve ncc robustly here so that no shell missing
# NCC_PATH can silently fall back to clang, and fail loudly if ncc is
# absent rather than producing a broken build.
if [[ -z "${NCC_PATH}" ]] ; then
    NCC_PATH=$(command -v ncc || true)
fi
if [[ -z "${NCC_PATH}" || ! -x "${NCC_PATH}" ]] ; then
    echo "ERROR: ncc not found. n00b must be built with ncc, not clang." >&2
    echo "       Put ncc on PATH, or set NCC_PATH=/abs/path/to/ncc." >&2
    exit 1
fi
export NCC_PATH
echo "build.sh: compiling n00b with ncc at ${NCC_PATH}"

# Optional compiler cache. Opt-in via N00B_CCACHE=1 because ncc is a custom
# compiler and ccache's correctness with it is not yet verified — a stale cache
# would silently produce a wrong build. When enabled and ccache is present, it
# is used as a launcher in front of ncc (CC="ccache <ncc>"); meson understands
# this launcher form. Toggling this only takes effect on a fresh configure
# (N00B_CLEAN=1 or a new build dir), since meson records CC at setup time.
N00B_CCACHE=${N00B_CCACHE:-0}
N00B_CC="${NCC_PATH}"
if [[ ${N00B_CCACHE} -ne 0 ]] ; then
    ccache_bin=$(command -v ccache || true)
    if [[ -z "${ccache_bin}" ]] ; then
        echo "build.sh: N00B_CCACHE set but ccache not found on PATH; building without it." >&2
    else
        N00B_CC="${ccache_bin} ${NCC_PATH}"
        echo "build.sh: compiler cache enabled (${ccache_bin})"
    fi
fi
export N00B_CC

# Ensure the macOS SDK root is set so the linker can find libSystem.
if [[ "$(uname -s)" == "Darwin" ]] && [[ -z "${SDKROOT}" ]] && command -v xcrun &>/dev/null; then
    export SDKROOT=$(xcrun --show-sdk-path 2>/dev/null)
fi

# ── Docker cross-compilation on macOS ────────────────────────────────────────
# On macOS, if Docker is available with an osxcross-enabled image, delegate
# to docker/cross-build.sh for cross-compilation. Set N00B_NATIVE=1 to
# skip this and build natively.
if [[ "$(uname -s)" == "Darwin" ]] && \
   [[ "${N00B_NATIVE}" == "0" ]] && \
   command -v docker &>/dev/null && \
   docker info &>/dev/null 2>&1 && \
   docker image inspect n00b-linux &>/dev/null 2>&1; then
    if docker run --rm n00b-linux test -d /usr/local/osxcross/bin 2>/dev/null; then
        echo "=== Docker cross-compilation (N00B_NATIVE=1 to override) ==="
        exec bash "${N00B_ROOT}/docker/cross-build.sh" macos-arm64
    fi
fi

# Pinned ncc revision. ncc and n00b co-evolve, so n00b builds against an exact
# ncc commit rather than ncc's moving main. Managed by pin-sync (.pin-sync.json,
# anchor NCC_REV_DEFAULT); override at build time with the NCC_REV env var.
NCC_REV_DEFAULT="7ab04e7079741fcbffab87f039c39970d2b8724b"
: "${NCC_REV:=${NCC_REV_DEFAULT}}"

function ensure_ncc_subproject {
    local ncc_src="${N00B_ROOT}/subprojects/ncc"

    if [[ -f "${ncc_src}/meson.build" ]] ; then
        return 0
    fi

    echo "=== Cloning ncc into subprojects/ncc ==="
    git clone https://github.com/crashappsec/ncc.git "${ncc_src}"

    if [[ -n "${NCC_REV:-}" ]] ; then
        git -C "${ncc_src}" checkout "${NCC_REV}"
    fi
}

function build_ncc {
    local ncc_src="${N00B_ROOT}/subprojects/ncc"
    local ncc_build="${ncc_src}/build_release"
    local ncc_bin="${ncc_build}/ncc"

    if [[ -x "${ncc_bin}" ]] && [[ "${N00B_BUILD_BOOTSTRAP:-0}" -eq 0 ]] ; then
        echo "=== Using cached ncc build ==="
        export NCC_PATH="${ncc_bin}"
        return 0
    fi

    echo "=== Building ncc from subprojects/ncc ==="
    local ncc_cc="${CC:-clang}"
    # Avoid circular dependency: if CC is already ncc, fall back to clang.
    if [[ "$(basename "${ncc_cc}")" == "ncc" ]] ; then
        ncc_cc="clang"
    fi

    if [[ "${N00B_BUILD_BOOTSTRAP:-0}" -ne 0 ]] && [[ -d "${ncc_build}" ]] ; then
        rm -rf "${ncc_build}"
    fi

    if [[ ! -d "${ncc_build}" ]] ; then
        CC="${ncc_cc}" meson setup --buildtype=release "${ncc_build}" "${ncc_src}"
    fi

    meson compile -C "${ncc_build}"
    export NCC_PATH="${ncc_bin}"
}

function ensure_ncc {
    # 1. Explicit override via NCC_PATH env var.
    if [[ -n "${NCC_PATH:-}" ]] && [[ -x "${NCC_PATH}" ]] ; then
        export NCC_PATH
        return 0
    fi

    # 2. In-tree subproject build, if already cached.  This is the
    #    binary the n00b build was tested against — strictly preferred
    #    over whatever ncc happens to be on PATH (which can drift from
    #    the in-tree build when developers update n00b without running
    #    `make install` for ncc).  Set N00B_USE_SYSTEM_NCC=1 to
    #    explicitly opt back into the PATH-resolved binary.
    if [[ "${N00B_USE_SYSTEM_NCC:-0}" -eq 0 ]] ; then
        local cached_ncc="${N00B_ROOT}/subprojects/ncc/build_release/ncc"
        if [[ -x "${cached_ncc}" ]] ; then
            export NCC_PATH="${cached_ncc}"
            return 0
        fi
    fi

    # 3. System ncc (opt-in via N00B_USE_SYSTEM_NCC=1, or fallback when the
    #    in-tree build hasn't been produced yet).  Prefer the stable
    #    /usr/local/bin/ncc over a bare `which ncc`: PATH may place an
    #    in-development ncc (e.g. ~/.local/bin/ncc) ahead of the installed
    #    one, which would compile the tree with an unreleased compiler.  Set
    #    NCC_PATH explicitly to override this default.
    local system_ncc
    if [[ -x /usr/local/bin/ncc ]] ; then
        system_ncc=/usr/local/bin/ncc
    else
        system_ncc=$(which ncc 2>/dev/null || true)
    fi
    if [[ -n "${system_ncc}" ]] ; then
        export NCC_PATH="${system_ncc}"
        return 0
    fi

    # 4. Build from subproject (cold start: no in-tree build, no system ncc).
    ensure_ncc_subproject
    build_ncc
}

function all_options {
    local s="-Dusing_build_script=true"

    if [[ ${N00B_BUILD_DEBUG:-0} -ne 0 ]] ; then
        s="${s} -Denable_debug=true"
    fi

    if [[ ${N00B_BUILD_DEV:-0} -ne 0 ]] ; then
        s="${s} -Ddev_mode=true"
    fi

    if [[ ${N00B_BUILD_LTO:-0} -ne 0 ]] ; then
        s="${s} -Denable_lto=true"
    fi

    # Build the ~470 test executables as part of the default compile target.
    # Off by default (the test suite isn't built during a normal `./build.sh`);
    # `meson test` still builds whatever it needs on demand. Set N00B_BUILD_TESTS=1
    # to force the whole suite into the `meson compile` step (e.g. CI prebuild).
    if [[ ${N00B_BUILD_TESTS:-0} -ne 0 ]] ; then
        s="${s} -Dbuild_tests=true"
    fi

    # Fast linker selection: auto (default, probe mold/lld), off, mold, or lld.
    # N00B_FAST_LINKER overrides the meson default only when set.
    if [[ -n "${N00B_FAST_LINKER:-}" ]] ; then
        s="${s} -Dfast_linker=${N00B_FAST_LINKER}"
    fi

    if [[ ${N00B_BUILD_GC_STATS:-0} -ne 0 ]] ; then
        s="${s} -Dshow_gc_stats=enabled"
    fi

    if [[ ${N00B_ROCS_TRACE:-0} -ne 0 ]] ; then
        s="${s} -Drocs_trace=true -Drocs_trace_every=${N00B_ROCS_TRACE_EVERY}"
    fi

    if [[ ${N00B_BUILD_MEMCHECK:-0} -ne 0 ]] ; then
        s="${s} -Duse_memcheck=on"
    fi

    if [[ ${N00B_BUILD_ASAN:-0} -ne 0 ]] ; then
        s="${s} -Duse_asan=enabled"
    fi

    if [[ ${N00B_BUILD_UBSAN:-0} -ne 0 ]] ; then
        s="${s} -Duse_ubsan=enabled"
    fi

    if [[ ${N00B_BUILD_MUSL:-0} -ne 0 ]] ; then
        s="${s} -Dusing_musl=true"
    fi

    if [[ ${N00B_BUILD_AWS:-0} -ne 0 ]] ; then
        s="${s} -Denable_aws=true"
    fi

    if [[ -n "${N00B_AWS_SHIM_PREFIX:-}" ]] ; then
        s="${s} -Daws_shim_prefix=${N00B_AWS_SHIM_PREFIX}"
    fi

    # Non-jj-workspace builds (the Docker container copies /src to /build; also
    # release tarballs) must override meson.build's jj VCS guard.
    if [[ ${N00B_SKIP_VCS_CHECK:-0} -ne 0 ]] ; then
        s="${s} -Dskip_vcs_check=true"
    fi

    s="${s} -Dunicode_allow_downloads=$(meson_bool "${N00B_UNICODE_ALLOW_DOWNLOADS}" N00B_UNICODE_ALLOW_DOWNLOADS)"
    s="${s} -Dunicode_strict_cache=$(meson_bool "${N00B_UNICODE_STRICT_CACHE}" N00B_UNICODE_STRICT_CACHE)"

    if [[ -n "${N00B_UNICODE_CACHE_DIR}" ]] ; then
        s="${s} -Dunicode_cache_dir=${N00B_UNICODE_CACHE_DIR}"
    fi

    echo "${s}"
}

function build_n00b {
   local build_dir=${1:-build_${N00B_BUILD_TYPE}}
   local compile_args=(-C "${build_dir}")
   if [[ -n "${N00B_JOBS}" ]] ; then
       compile_args+=(-j "${N00B_JOBS}")
   fi
   # Keep-going: surface ALL independent build failures in one pass (Linux-port
   # grind) instead of stopping at the first error.
   if [[ ${N00B_KEEP_GOING:-0} -ne 0 ]] ; then
       compile_args+=(--ninja-args=-k0)
   fi
   if [[ -n "${N00B_BUILD_TARGETS}" ]] ; then
       local requested_build_targets=()
       read -r -a requested_build_targets <<< "${N00B_BUILD_TARGETS}"
       compile_args+=("${requested_build_targets[@]}")
   fi

   if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d ${build_dir} ]] ; then
       rm -rf ${build_dir}
   fi

   # GC type-map dictionary aggregation. We slot a thin wrapper in as OBJC so the
   # dictionary gets aggregated per-executable AT LINK: meson links executables
   # with the ObjC compiler (libn00b.a carries the Cocoa bridge), not with ncc,
   # so ncc's own link-stage gcmap hook never fires. The wrapper passes ObjC
   # COMPILES straight through to Apple clang, and on LINK runs
   # `ncc --ncc-gcmap-emit` over the link inputs (the executable's own TUs +
   # libn00b.a) and appends the generated dictionary object. See the wrapper and
   # the n00b_app_kwargs note in meson.build. If the wrapper or ncc is
   # unavailable it degrades to a transparent link (conservative GC scan, safe).
   #
   # These NCC_GCMAP_* vars are read by the wrapper at LINK time, so export them
   # unconditionally (not only when (re)configuring), so plain incremental
   # rebuilds via build.sh re-run the wrapper with aggregation enabled. OBJC
   # itself is recorded by meson only at setup, so it is set in the setup branch.
   _real_objc=""
   _gcmap_wrapper="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/ncc-gcmap-objc-link-wrapper.sh"
   if [[ "$(uname -s)" == "Darwin" ]] && command -v xcrun &>/dev/null; then
       _real_objc=$(xcrun --find clang 2>/dev/null)
       if [[ -x "${_gcmap_wrapper}" ]]; then
           export NCC_GCMAP_REAL_OBJC="${_real_objc}"
           export NCC_GCMAP_NCC="${NCC_PATH}"
           export NCC_GCMAP_INCLUDE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/include"
       fi
   fi

   if [[ ! -d ${build_dir} ]] ; then
       # OBJC must point to Apple's clang (with sysroot) for the Cocoa backend;
       # the LLVM clang at /usr/local/bin lacks macOS SDK paths. Use the gcmap
       # link wrapper when available (see above), else Apple clang directly.
       if [[ -n "${_real_objc}" ]]; then
           if [[ -x "${_gcmap_wrapper}" ]]; then
               export OBJC="${_gcmap_wrapper}"
           else
               export OBJC="${_real_objc}"
           fi
       fi
       CC="${N00B_CC}" meson setup --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
       if [[ $? -ne 0 ]] ; then
           echo "Build setup failed."
           exit 1
       fi
   fi

   if ! meson compile "${compile_args[@]}"; then
       echo "Build compile failed."
       exit 1
   fi

   if [[ ${N00B_TEST} -ne 0 ]] ; then
       local meson_test_args=(--print-errorlogs --timeout-multiplier 3)
       if [[ ${N00B_TEST_ALL} -eq 0 && -z "${N00B_TESTS}" && -z "${N00B_TEST_NO_SUITES}" ]] && ! test_suite_requested long ; then
           add_test_no_suite long
       fi
       if [[ -n "${N00B_JOBS}" ]] ; then
           meson_test_args+=(--num-processes "${N00B_JOBS}")
       fi
       if [[ ${N00B_TEST_FAIL_FAST} -ne 0 ]] ; then
           meson_test_args+=(--maxfail "${N00B_TEST_FAIL_FAST}")
       fi
       if [[ -n "${N00B_BUILD_TARGETS}" && -n "${N00B_TESTS}" ]] ; then
           meson_test_args+=(--no-rebuild)
       fi
       if [[ -n "${N00B_TEST_SUITES}" ]] ; then
           local requested_suites=()
           read -r -a requested_suites <<< "${N00B_TEST_SUITES}"
           local suite
           for suite in "${requested_suites[@]}" ; do
               meson_test_args+=(--suite "${suite}")
           done
       fi
       if [[ -n "${N00B_TEST_NO_SUITES}" ]] ; then
           local skipped_suites=()
           read -r -a skipped_suites <<< "${N00B_TEST_NO_SUITES}"
           local no_suite
           for no_suite in "${skipped_suites[@]}" ; do
               meson_test_args+=(--no-suite "${no_suite}")
           done
       fi
       if [[ -n "${N00B_TESTS}" ]] ; then
           local requested_tests=()
           read -r -a requested_tests <<< "${N00B_TESTS}"
           meson_test_args+=("${requested_tests[@]}")
       fi
       meson test -C "${build_dir}" "${meson_test_args[@]}"
       if [[ $? -ne 0 ]] ; then
           echo "Tests failed."
           exit 1
       fi
   fi

   if [[ ${N00B_DOCS} -ne 0 ]] ; then
       local docs_args=(-C "${build_dir}")
       if [[ -n "${N00B_JOBS}" ]] ; then
           docs_args+=(-j "${N00B_JOBS}")
       fi
       docs_args+=(docs)
       meson compile "${docs_args[@]}"
       if [[ $? -ne 0 ]] ; then
           echo "Documentation generation failed."
           exit 1
       fi
       echo "Documentation generated in ${build_dir}/docs/html/"
   fi
}

# ============================================================================
# Cross-compilation support
#
# N00B_CROSS=all              — cross-compile for all targets with available toolchains
# N00B_CROSS=linux-x86_64     — cross-compile for one specific target
# ============================================================================

function clang_version_at_least_22_1 {
    local clang_bin=$1
    local version_line
    version_line=$("${clang_bin}" --version 2>/dev/null | sed -n '1p')

    if [[ ! "${version_line}" =~ clang[[:space:]]+version[[:space:]]+([0-9]+)\.([0-9]+)\.([0-9]+) ]]; then
        return 1
    fi

    local major=${BASH_REMATCH[1]}
    local minor=${BASH_REMATCH[2]}

    if (( major > 22 )); then
        return 0
    fi

    if (( major == 22 && minor >= 1 )); then
        return 0
    fi

    return 1
}

function find_windows_toolchain {
    WINDOWS_CLANG=""
    WINDOWS_AR=""
    WINDOWS_STRIP=""
    WINDOWS_WINDRES=""

    local candidate_root=""
    if [[ -n "${N00B_LLVM_MINGW:-}" ]]; then
        candidate_root="${N00B_LLVM_MINGW}"
    elif [[ -n "${LLVM_MINGW:-}" ]]; then
        candidate_root="${LLVM_MINGW}"
    fi

    if [[ -n "${candidate_root}" ]]; then
        local bin_dir="${candidate_root}/bin"
        WINDOWS_CLANG="${bin_dir}/clang"
        WINDOWS_AR="${bin_dir}/llvm-ar"
        WINDOWS_STRIP="${bin_dir}/llvm-strip"
        WINDOWS_WINDRES="${bin_dir}/llvm-windres"
    else
        local clang_path=""
        clang_path=$(command -v x86_64-w64-mingw32-clang 2>/dev/null || true)
        if [[ -z "${clang_path}" ]]; then
            clang_path=$(command -v clang 2>/dev/null || true)
        fi
        if [[ -n "${clang_path}" ]]; then
            local bin_dir
            bin_dir=$(cd "$(dirname "${clang_path}")" && pwd)
            WINDOWS_CLANG="${clang_path}"
            WINDOWS_AR="${bin_dir}/llvm-ar"
            WINDOWS_STRIP="${bin_dir}/llvm-strip"
            WINDOWS_WINDRES="${bin_dir}/llvm-windres"
        fi
    fi

    if [[ ! -x "${WINDOWS_CLANG}" ]] ; then
        local msg="Windows cross build requires llvm-mingw clang. Set LLVM_MINGW or N00B_LLVM_MINGW to the llvm-mingw install directory."
        if [[ "${N00B_CROSS}" == "all" ]] ; then
            echo "  [SKIP] windows-x86_64 — ${msg}"
            return 1
        fi
        fail "${msg}"
    fi

    if [[ ! -x "${WINDOWS_AR}" ]] ; then
        local msg="Windows cross build requires llvm-ar from the llvm-mingw toolchain. Set LLVM_MINGW or N00B_LLVM_MINGW."
        if [[ "${N00B_CROSS}" == "all" ]] ; then
            echo "  [SKIP] windows-x86_64 — ${msg}"
            return 1
        fi
        fail "${msg}"
    fi

    if [[ ! -x "${WINDOWS_STRIP}" ]] ; then
        local msg="Windows cross build requires llvm-strip from the llvm-mingw toolchain. Set LLVM_MINGW or N00B_LLVM_MINGW."
        if [[ "${N00B_CROSS}" == "all" ]] ; then
            echo "  [SKIP] windows-x86_64 — ${msg}"
            return 1
        fi
        fail "${msg}"
    fi

    if [[ ! -x "${WINDOWS_WINDRES}" ]]; then
        WINDOWS_WINDRES=""
    fi

    if ! clang_version_at_least_22_1 "${WINDOWS_CLANG}"; then
        "${WINDOWS_CLANG}" --version || true
        local msg="Windows cross build requires llvm-mingw Clang 22.1.0 or newer."
        if [[ "${N00B_CROSS}" == "all" ]] ; then
            echo "  [SKIP] windows-x86_64 — ${msg}"
            return 1
        fi
        fail "${msg}"
    fi
}

function write_windows_cross_file {
    local build_dir=$1
    local cross_file="/tmp/n00b-${build_dir}-$$.cross"

    {
        echo "[binaries]"
        echo "c = ['${NCC_PATH}', '--target=x86_64-w64-windows-gnu']"
        echo "ar = '${WINDOWS_AR}'"
        echo "strip = '${WINDOWS_STRIP}'"
        if [[ -n "${WINDOWS_WINDRES}" ]]; then
            echo "windres = '${WINDOWS_WINDRES}'"
        fi
        echo
        echo "[properties]"
        echo "needs_exe_wrapper = true"
        echo
        echo "[host_machine]"
        echo "system = 'windows'"
        echo "cpu_family = 'x86_64'"
        echo "cpu = 'x86_64'"
        echo "endian = 'little'"
    } > "${cross_file}"

    echo "${cross_file}"
}

function cross_compile_target {
    local cross_file=$1
    local target_name=$(basename ${cross_file} .cross)
    local build_dir="build_cross_${target_name}"
    local effective_cross_file="${cross_file}"
    local cross_cc=""

    if [[ "${target_name}" == "windows-x86_64" ]] ; then
        if ! find_windows_toolchain ; then
            return 0
        fi
        effective_cross_file=$(write_windows_cross_file "${build_dir}")
        cross_cc="${NCC_PATH}"
    else
        # Extract the C compiler path from the cross file.
        cross_cc=$(python3 -c "
import ast, configparser, os, pathlib
p = pathlib.Path('${cross_file}')
cp = configparser.ConfigParser()
cp.read(p)
tc = cp.get('constants', 'toolchain', fallback='/usr').strip().strip(\"'\")
c_val = cp.get('binaries', 'c', fallback='').strip()
if c_val.startswith('toolchain / '):
    leaf = c_val[len('toolchain / '):].strip()
    try:
        leaf = ast.literal_eval(leaf)
    except Exception:
        leaf = leaf.strip(\"'\")
    c_val = os.path.join(tc, leaf)
elif c_val.startswith('['):
    try:
        c_val = ast.literal_eval(c_val)[0]
    except Exception:
        c_val = ''
else:
    try:
        c_val = ast.literal_eval(c_val)
    except Exception:
        c_val = c_val.strip(\"'\")
print(c_val)
" 2>/dev/null)
    fi

    if [[ -z "${cross_cc}" ]] || [[ ! -x "${cross_cc}" ]] ; then
        echo "  [SKIP] ${target_name} — cross-compiler not found: ${cross_cc:-<empty>}"
        return 0
    fi

    echo "  [BUILD] ${target_name} (${cross_cc})"

    if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d ${build_dir} ]] ; then
        rm -rf ${build_dir}
    fi

    if [[ ! -d ${build_dir} ]] ; then
        if [[ -n "${WINDOWS_CLANG:-}" ]]; then
            NCC_COMPILER=${WINDOWS_CLANG} \
            CC=${NCC_PATH} \
            meson setup --cross-file ${effective_cross_file} \
                --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
        else
            CC=${NCC_PATH} \
            meson setup --cross-file ${effective_cross_file} \
                --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
        fi
        if [[ $? -ne 0 ]] ; then
            echo "  [FAIL] ${target_name} — meson setup failed"
            return 1
        fi
    fi

    if [[ -n "${WINDOWS_CLANG:-}" ]]; then
        NCC_COMPILER=${WINDOWS_CLANG} meson compile -C ${build_dir}
    else
        meson compile -C ${build_dir}
    fi
    if [[ $? -ne 0 ]] ; then
        echo "  [FAIL] ${target_name} — compile failed"
        return 1
    fi

    echo "  [OK]   ${target_name}"
    return 0
}

function cross_compile {
    echo "Cross-compiling..."
    local failed=0

    if [[ "${N00B_CROSS}" == "all" ]] ; then
        for cf in ${N00B_ROOT}/cross/*.cross ; do
            cross_compile_target "${cf}" || failed=1
        done
    else
        local cf="${N00B_ROOT}/cross/${N00B_CROSS}.cross"
        if [[ ! -f "${cf}" ]] ; then
            echo "Cross file not found: ${cf}"
            echo "Available targets:"
            ls ${N00B_ROOT}/cross/*.cross 2>/dev/null | while read f; do
                echo "  $(basename $f .cross)"
            done
            exit 1
        fi
        cross_compile_target "${cf}" || failed=1
    fi

    if [[ ${failed} -ne 0 ]] ; then
        echo "Some cross-compilation targets failed."
        exit 1
    fi
}

ensure_ncc
if [[ -n "${N00B_CROSS}" ]] ; then
    cross_compile
else
    build_n00b "${N00B_BUILD_ARGS[@]}"
fi
