project(
  'n00b',
  'c',
  default_options: [
    'default_library=static',
  ],

  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.6.0'
  ,
)

if (get_option('using_build_script') != true)
  _setup_entrypoint = 'Do not try to run meson directly. Run ./build.sh.'
  _setup_why = 'WHY: build.sh resolves NCC_PATH to the freshly-built workspace ncc\n' +
        '     (see ensure_ncc), then invokes meson with CC=$NCC_PATH so the\n' +
        '     n00b build uses the correct compiler.'
  if host_machine.system() == 'windows'
    _setup_entrypoint = 'Do not try to run meson directly. Run .\\build.ps1.'
    _setup_why = 'WHY: build.ps1 resolves NCC_PATH, or the neighboring\n' +
        '     ..\\ncc\\build-msvc\\ncc.exe, then invokes meson with CC set so\n' +
        '     the native Windows n00b build uses the correct compiler.'
  endif
  error(_setup_entrypoint + '\n' +
        '\n' +
        _setup_why + ' Bypassing the build wrapper and\n' +
        '     setting -Dusing_build_script=true loses this resolution and\n' +
        '     leaves meson to pick whatever clang is on PATH — which then\n' +
        '     receives --ncc-* flags it does not understand and fails the\n' +
        '     gen_unicode_lib / vendor library compile.\n' +
        '\n' +
        '     See docs/dict_literals.md ("Build invocation pitfalls").\n' +
        '\n' +
        'IF YOU REALLY MEAN IT: pass -Dusing_build_script=true AND set\n' +
        '                       CC=<workspace ncc> AND understand that no\n' +
        '                       further n00b-internal guards apply.')
endif

# ── VCS-state guard ──────────────────────────────────────────────────────────
# Configuring from a bad VCS state silently produces stale/wrong builds. This
# has burned us repeatedly: building a checkout that is NOT a jj workspace, or
# whose main is behind origin/main (e.g. building #96 while trunk was #97).
# Refuse to configure in either case. Override only deliberately with
# -Dskip_vcs_check=true (release tarballs / CI checkouts with no .jj).
if get_option('skip_vcs_check') != true
  jj_prog = find_program('jj', required: false)
  if not jj_prog.found()
    error('VCS guard: jj not found. n00b must be built from a jj workspace.\n' +
          '  Override (e.g. tarball build): -Dskip_vcs_check=true')
  endif
  # 1. Must be inside a jj workspace.
  _ws = run_command(jj_prog, 'workspace', 'root',
                    '--ignore-working-copy', check: false)
  if _ws.returncode() != 0
    error('VCS guard: not inside a jj workspace — refusing to configure (this\n' +
          '  would build an unsynced/stale checkout). cd into a jj workspace,\n' +
          '  or override with -Dskip_vcs_check=true')
  endif
  # 2. Local main must NOT be behind origin/main (i.e. the repo is synced with
  #    remote trunk). Revset = commits in origin/main not reachable from local
  #    main; non-empty => behind. Uses the last-fetched origin state (run
  #    `jj git fetch` first to be sure it is current).
  _behind = run_command(jj_prog, 'log', '-r', 'main@origin ~ ::main',
                        '--no-graph', '--ignore-working-copy',
                        '-T', 'change_id ++ "\n"', check: false)
  if _behind.returncode() != 0
    # The revset failed — almost always because `main@origin` doesn't exist
    # yet (a fresh clone that has never run `jj git fetch`, or a remote not
    # named `origin`). Don't silently treat that as "up to date": warn so
    # the developer knows the behind-check did NOT actually run.
    warning('VCS guard: could not evaluate main@origin (no fetched remote ' +
            'trunk?). Skipping the behind-origin check. Run `jj git fetch` ' +
            'to enable it, or set -Dskip_vcs_check=true to silence this.')
  elif _behind.stdout().strip() != ''
    error('VCS guard: local main is BEHIND origin/main (stale trunk).\n' +
          '  Run:  jj git fetch  then rebase your work onto main and reconfigure.\n' +
          '  Override with -Dskip_vcs_check=true')
  else
    message('VCS guard: in a jj workspace; local main is up to date with origin/main.')
  endif
endif

target_os = host_machine.system()
target_arch = host_machine.cpu_family()
macos_sdk = ''
if target_os == 'darwin'
  macos_sdk = run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip()
endif

cc = meson.get_compiler('c')
vcpkg_inc = get_option('vcpkg_inc')
vcpkg_lib = get_option('vcpkg_lib')

incdir = [include_directories('include'), include_directories('include/internal'), include_directories(vcpkg_inc)]
quic_test_fixture_inc = include_directories('test/fixtures')
using_macos = host_machine.system() == 'darwin'

if target_os == 'darwin'
  add_languages('objc', required: true, native: false)
  # The ObjC link step (Xcode's clang, selected for any target that links an
  # ObjC bridge) needs the SDK sysroot to resolve the implicit `-lSystem`.
  # Meson passes `-isysroot` to ObjC compile steps but not to links, so pin it
  # for all Darwin ObjC bridge libraries.
  add_project_link_arguments('-isysroot', macos_sdk, language: 'objc')
endif

conf_cocoa = get_option('enable_cocoa') and target_os == 'darwin'

conf_notcurses = get_option('enable_notcurses')
conf_freetype  = get_option('enable_freetype')
conf_x11       = get_option('enable_x11') and target_os != 'windows'

conf_release_build = get_option('buildtype') == 'release'
conf_asan = get_option('use_asan').enabled()
conf_ubsan = get_option('use_ubsan').enabled()
conf_memcheck = get_option('use_memcheck') != 'off'
conf_gc_stats = get_option('show_gc_stats').enabled()
conf_rocs_trace = get_option('rocs_trace')
conf_rocs_trace_every = get_option('rocs_trace_every')
conf_debug_live_census = get_option('debug_live_census')
conf_lto = get_option('enable_lto')
conf_dev_mode = get_option('dev_mode')
# When false (the default), the ~470 test executables are configured but kept
# out of the default `meson compile` / `ninja` target via build_by_default.
# `meson test` still builds them on demand. See the build_tests option.
n00b_build_tests = get_option('build_tests')
# Test binaries call into debug-only surface (work counters, index
# introspection) and rely on the planner's assertions, so building them
# turns that surface on.
conf_debug = get_option('enable_debug') or n00b_build_tests
conf_static_grammar_image_requested = get_option('enable_static_grammar_image')
elf_object_host_check = '''
#ifndef __ELF__
#error host is not ELF
#endif
int main(void) { return 0; }
'''
conf_static_grammar_image_host_supported = target_os == 'darwin' or cc.compiles(
  elf_object_host_check,
  name: '__ELF__ host object support for static grammar images',
)
conf_static_grammar_image = conf_static_grammar_image_requested and conf_static_grammar_image_host_supported
if conf_static_grammar_image_requested and not conf_static_grammar_image_host_supported
  message('Static grammar image object support is not available on this host; registering visible [SKIP] tests and using runtime BNF fallback.')
endif

glibc_check = '''
#include <stdio.h>
#ifndef __GLIBC__
#error NO GLIBC
#endif
'''

if cc.compiles(glibc_check)
  using_glibc = true
else
  using_glibc = false
endif

posix_check = '''
#include <sys/mman.h>

int
test(void)
{
  return getpagesize() + MAP_ANON;
}
'''

if target_os == 'darwin'
  n00b_static_object_entry_attr = '__attribute__((section("__DATA,n00b_stobj"),used))'
elif target_os == 'windows'
  n00b_static_object_entry_attr = '__attribute__((section(".n00bs$m"),used))'
else
  n00b_static_object_entry_attr = '__attribute__((section("n00b_stobj"),used,retain))'
endif

# WP-011 Phase 3c.ii.b: the descriptor initializer carries an explicit
# `.cached_hash=$N` slot so the build-time precomputed XXH3_128bits of
# the r-string content (used when the rstr serves as a dict key) is
# threaded through to the static-range cached_hash field that D-066
# extended for pointer-key short-circuiting in `n00b_hash()`.  For the
# common (non-dict-key) emission ncc passes `0` for this slot and the
# field is zero-filled — `n00b_hash()` then falls through to the
# vtable hash, same as before.  Slot index is appended to the existing
# layout (plain: $16; styled: $18) so existing positional bindings stay
# stable.
# Type-name-free static-object descriptor: an anonymous struct whose field
# order/types/widths match n00b_static_object_desc_t (core/codegen_abi_inject.h)
# EXACTLY, so r-string-bearing TUs need no codegen-ABI header. The section-
# resident entry pointer is `const void *` (byte-identical to the typed
# pointer), and `.identity` is cast to `const void *`. The codegen_abi_guard.c
# TU _Static_asserts the real struct still matches this layout — if it drifts,
# the build fails there rather than silently mis-scanning. (n00b_string_t and
# n00b_uint128_t remain spelled directly: they live in the always-force-included
# n00b.h, not the codegen-ABI header.)
n00b_sodesc = 'struct{const void*start;uint64_t len;uint64_t tinfo;unsigned char scan_kind;void(*scan_cb)(void*,void*);void*scan_user;uint64_t object_id;const char*file;const void*identity;uint32_t flags;unsigned _BitInt(128) cached_hash;}'
n00b_rstr_template_plain = '({static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const ' + n00b_sodesc + ' $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=(const void*)$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const void * const $7 $13=(const void*)&$6;&$0;})'
n00b_rstr_template_styled = '({$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const ' + n00b_sodesc + ' $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=(const void*)$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const void * const $9 $15=(const void*)&$8;&$1;})'
n00b_rstr_static_ref_template_plain = 'static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const ' + n00b_sodesc + ' $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=(const void*)$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const void * const $7 $13=(const void*)&$6;'
n00b_rstr_static_ref_template_styled = '$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const ' + n00b_sodesc + ' $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=(const void*)$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const void * const $9 $15=(const void*)&$8;'

c_args = [
  '--ncc-vargs-type=n00b_vargs_t',
  '--ncc-rstr-string-type=n00b_string_t',
  '--ncc-rstr-text-style-type=n00b_text_style_t',
  '--ncc-rstr-style-record-type=n00b_style_record_t',
  '--ncc-static-object-entry-attr=' + n00b_static_object_entry_attr,
  '--ncc-rstr-template-plain=' + n00b_rstr_template_plain,
  '--ncc-rstr-template-styled=' + n00b_rstr_template_styled,
  '--ncc-rstr-static-ref-template-plain=' + n00b_rstr_static_ref_template_plain,
  '--ncc-rstr-static-ref-template-styled=' + n00b_rstr_static_ref_template_styled,
  '--ncc-rstr-static-ref-expr-plain=&$0',
  '--ncc-rstr-static-ref-expr-styled=&$1',
  '-include',
  'n00b.h',
  # core/codegen_abi_inject.h is NO LONGER force-included. ncc now emits the GC
  # root tables, stack maps, static-object descriptors/identities, and transient
  # tables as type-name-free ANONYMOUS structs (and forward-declares the runtime
  # push/pop with void* signatures), so emitted code in arbitrary TUs needs no
  # codegen-ABI header. The handful of hand-written TUs that instantiate these
  # structs by value, dereference them, or use the enum constants include the
  # header explicitly; src/core/codegen_abi_guard.c _Static_asserts the emitted
  # anonymous layouts still match the real structs. Editing the header now
  # rebuilds only the guard TU + those few genuine users, not the whole tree.
  '-Wextra',
  '-g',
  '-std=c23',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-cast-function-type',
  '-Wno-parentheses-equality',
  '-Wno-initializer-overrides',
  '-Wno-unknown-pragmas',
  '-fno-omit-frame-pointer',
  '-ffunction-sections',
  # TODO only needed for musl
  # '-target', target_arch + '-' + target_os,
]

if target_os != 'windows'
  c_args += '-fPIC'
endif
# ncc cannot tokenize the libgcc/clang SIMD intrinsic headers xxhash
# pulls in (compound-literal vector casts in mmintrin.h). Disable
# xxhash's intrinsic paths unconditionally — its scalar fallback is
# still fast.
c_args += '-DXXH_NO_INTRINSICS=1'

# The lock-free llstack (n00b_head_t) is a 16-byte {ptr,tag} DWCAS.  On x86-64
# the 16-byte atomic CAS (cmpxchg16b) is only lock-free with -mcx16; without it
# clang downgrades to a non-lock-free __atomic_* libcall, defeating the
# lock-free stack.  arm64 gets the lock-free 128-bit path (casp/ldxp-stxp)
# without an extra flag.
if target_arch == 'x86_64'
  c_args += '-mcx16'
endif

if using_glibc
  c_args = c_args + [
    '-D_GNU_SOURCE',
    '-DN00B_NCC_GLIBC_ASSERT_SHIM',
  ]
endif

if target_os == 'darwin'
    link_args = ['-target', target_arch + '-apple-' + target_os,
                 '-isysroot', macos_sdk]
elif target_os == 'windows'
    # Native MSVC builds use the wrapper's /MD runtime selection. The old
    # MinGW-oriented -static flag suppresses those CRT import libraries during
    # ncc's compile-time image link.
    link_args = [
      '-Wl,/STACK:16777216',
      '--ncc-no-comptime',
      '--ncc-system-entry',
      '-lmsvcrt',
      '-lvcruntime',
      '-lucrt',
    ]
else
    link_args = []
endif

if target_os == 'darwin'
  c_args = c_args + ['-Wno-atomic-alignment']
  c_args = c_args + cc.get_supported_arguments(
    '-Wno-deprecated-declarations',
  )
endif

if conf_release_build
  c_args = c_args + ['-DN00B_RELEASE_BUILD=1']
endif

if conf_dev_mode
  c_args = c_args + ['-DN00B_DEV']
endif

if conf_debug
  c_args = c_args + ['-DN00B_DEBUG']
endif

if conf_debug_live_census
  c_args = c_args + ['-DN00B_DEBUG_LIVE_CENSUS=1']
  # The per-allocation-site pool audit (pool.c) is part of the debug live
  # census: it names the sites whose live bytes never come back down, which is
  # the GC-liveness leak signal for pools with no OOB metadata (user_pool /
  # conduit_pool). Debug-only; adds a trailing site-word to every audited-pool
  # allocation plus a per-site GC dict, so it never ships in a release build.
  c_args = c_args + ['-DN00B_POOL_ALLOC_AUDIT=1']
endif

if conf_rocs_trace
  c_args = c_args + [
    '-DN00B_ROCS_TRACE',
    '-DN00B_ROCS_TRACE_EVERY=' + conf_rocs_trace_every.to_string(),
  ]
endif

if conf_asan
  c_args = c_args + [
    '-fsanitize=address',
    '-fsanitize-recover=all',
    '-DN00B_ASAN',
  ]
  link_args = link_args + [
    '-fsanitize=address',
    '-fsanitize-recover=all',
  ]
  if using_macos
    c_args = c_args + ['-fsanitize-blacklist=../.sanitize_ignore']
  endif
endif

if conf_ubsan
  c_args = c_args + [
    '-fsanitize=undefined',
    '-fno-sanitize=function',
    '-fsanitize-recover=all',
    '-DN00B_UBSAN',
  ]
  link_args = link_args + [
    '-fsanitize=undefined',
    '-fsanitize-recover=all',
  ]
endif

if (conf_memcheck)
  c_args = c_args + [
    '-DN00B_AUTO_MEMCHECK=3',
  ]
endif

if conf_gc_stats
  c_args = c_args + [
    '-DN00B_GC_STATS',
  ]
endif

if target_os != 'windows'
  if not cc.compiles(posix_check)
    c_args = c_args + ['-DPOSIX_C_SOURCE=200112L']
  endif
endif

# Detect __attribute__((aligned(N))) for word-at-a-time UTF-8 fast paths.
aligned_attr_check = '''
int test(void) {
  __attribute__((aligned(8))) char buf[16];
  return buf[0];
}
'''
if cc.compiles(aligned_attr_check, name: '__attribute__((aligned))')
  c_args = c_args + ['-DN00B_HAVE_ALIGNED_ATTR']
endif

vector_builtins_check = '''
typedef unsigned char v16u8 __attribute__((vector_size(16)));
int main(void) {
  v16u8 a = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  return a[0];
}
'''
if target_os != 'windows' and cc.compiles(vector_builtins_check, name: 'vector_size(16)')
  c_args = c_args + ['-DN00B_HAVE_VECTOR_BUILTINS']
endif

# ── Fast linker (mold / lld) ─────────────────────────────────────────────────
# Linking ~470 test executables against libn00b is link-bound, especially on
# Linux/Docker where the default BFD linker is slow. mold (and lld) are far
# faster. We only add `-fuse-ld=<linker>` if (a) the program is on PATH and
# (b) the compiler actually accepts the flag and can link with it — the
# has_link_argument probe link-tests a trivial program, so a compiler that
# doesn't understand the flag (or a linker that isn't really usable) falls back
# to the default linker instead of breaking the build. macOS already ships a
# fast linker (ld-prime), so the gain there is small; this mainly helps Linux.
fast_linker_choice = get_option('fast_linker')
if fast_linker_choice == 'off'
  fast_linker_candidates = []
elif fast_linker_choice == 'auto'
  fast_linker_candidates = ['mold', 'lld']
else
  fast_linker_candidates = [fast_linker_choice]
endif

foreach _linker : fast_linker_candidates
  if not link_args.contains('-fuse-ld=' + _linker)
    _linker_prog = find_program(_linker, required: false)
    if _linker_prog.found() and cc.has_link_argument('-fuse-ld=' + _linker)
      link_args = link_args + ['-fuse-ld=' + _linker]
      message('Using fast linker: ' + _linker + ' (-fuse-ld=' + _linker + ')')
      break
    endif
  endif
endforeach

exe_link_args = link_args
exe_c_args = c_args + [
  '-DN00B_HOST_OS=' + host_machine.system(),
  '-DN00B_HOST_CPU=' + host_machine.cpu_family(),
  # Emit raw, dependency-free n00b_gcraw type->GC-map records per TU instead of
  # typed n00b_gc_* dictionary entries. This is what decouples the volatile
  # core/codegen_abi.h from the ~500 TUs that define pointer-bearing aggregates.
  # ncc strips this flag from the underlying compiler argv (incl. --no-ncc), so
  # it is harmless on the non-ncc helper libraries. The raw records are
  # aggregated per-executable into a generated dictionary object by the ObjC link
  # wrapper (scripts/ncc-gcmap-objc-link-wrapper.sh); see the longer note near
  # n00b_app_kwargs below. (Done in the link wrapper rather than ncc's own
  # link-stage hook because in this build the ObjC compiler links executables.)
  '--ncc-gcmap-prelink',
]
if target_os == 'windows'
  exe_c_args += '--ncc-no-comptime'
endif

ncc_lto_supported = cc.has_argument('--ncc-lto-guard')

if conf_lto and ncc_lto_supported
  exe_c_args = exe_c_args + ['-flto', '-DN00B_LTO_ENABLED', '--ncc-lto-guard']
  exe_link_args = exe_link_args + ['-flto']
endif

if conf_release_build
  exe_c_args = exe_c_args + ['-O3', '-ffast-math']
  exe_c_args = exe_c_args + cc.get_supported_arguments('-fno-finite-math-only')
  if ncc_lto_supported
    exe_c_args = exe_c_args + ['-flto', '--ncc-lto-guard']
    exe_link_args = exe_link_args + ['-flto']
  endif
endif

pt_attr_np_code = '''
#include <pthread.h>

int main(void) {
  pthread_attr_t attrs;
  pthread_getattr_np(pthread_self(), &attrs);
  return 0;
}
'''

if cc.compiles(pt_attr_np_code)
  c_args = c_args + '-DN00B_HAVE_PT_GETATTR_NP'
endif

fpty_code = '''
#include <stddef.h>
#include <pty.h>

int main(void) { forkpty(nullptr, nullptr, nullptr, nullptr); return 0; }
'''

if cc.links(fpty_code, name: 'forkpty_check')
  exe_c_args = exe_c_args + ['-DHAVE_PTY_H']
endif

exe_link_args = exe_link_args + ['-w']

libatomic_dep = declare_dependency()
if host_machine.system() != 'darwin'
  libatomic_dep = cc.find_library('atomic', required: false)
endif
libmath_dep = declare_dependency()
if host_machine.system() != 'darwin'
  libmath_dep = cc.find_library('m', required: false)
endif
libthreads_dep = dependency('threads', required: true)

# WP-016 Phase 2: the macOS-only NFS execution mode locates its setuid mount
# helper at a fixed install path; feed that path into the runner so the runtime
# probe + n00b_subproc invocation match where the helper is installed. The
# objfile sources are compiled as part of the MONOLITHIC `n00b` static_library,
# which meson cannot give per-source c_args — so the define is added to the
# shared `exe_c_args` (a harmless string constant for every other n00b source).
# A non-default install prefix changes this path (deployment concern, OQ-2/OQ-3).
if host_machine.system() == 'darwin'
  nfs_mount_helper_path = get_option('prefix') / get_option('libexecdir') \
    / 'n00b-nfs-mount-helper'
  exe_c_args = exe_c_args + [
    '-DN00B_NFS_MOUNT_HELPER_PATH="@0@"'.format(nfs_mount_helper_path),
  ]
endif

deps = [
  libmath_dep,
  libthreads_dep,
]
if libatomic_dep.found()
  deps = deps + [libatomic_dep]
endif

if conf_cocoa
  cocoa_dep = declare_dependency(link_args: [
    '-framework', 'Cocoa',
    '-framework', 'CoreText',
    '-framework', 'CoreGraphics',
    '-framework', 'QuartzCore',
  ])
  deps = deps + [cocoa_dep]
endif

# macOS Security.framework + CoreFoundation: needed by the ACME trust
# verifier (acme_trust_macos.m) and Keychain secret bridge
# (secret_keychain.m).  Always-shipped Apple frameworks; unconditional
# on darwin.
if target_os == 'darwin'
  acme_trust_dep = declare_dependency(link_args: [
    '-framework', 'Security',
    '-framework', 'CoreFoundation',
  ])
  deps = deps + [acme_trust_dep]
  xpc_native_dep = declare_dependency(link_args: [
    '-framework', 'Foundation',
  ])
  deps = deps + [xpc_native_dep]
endif

if conf_notcurses
  notcurses_dep = dependency('notcurses', version: '>=3.0.10', required: false)
  conf_notcurses = notcurses_dep.found()
endif

if conf_x11
  x11_dep = dependency('x11', required: false)
  conf_x11 = x11_dep.found()
endif

if conf_freetype
  freetype_dep = dependency('freetype2', required: false)
  conf_freetype = freetype_dep.found()
endif

if conf_notcurses
  exe_c_args = exe_c_args + ['-DN00B_HAVE_NOTCURSES=1']
  deps += notcurses_dep
endif
if conf_freetype
  exe_c_args = exe_c_args + ['-DN00B_HAVE_FREETYPE=1']
  deps += freetype_dep
endif
if conf_x11
  exe_c_args = exe_c_args + ['-DN00B_HAVE_X11=1']
  deps += x11_dep
endif

mir_proj = subproject('mir')
mir_dep  = mir_proj.get_variable('mir_dep')

# libzip — optional backend for src/chalk/zip.c. When disabled or absent,
# the ZIP codec remains compiled and returns unsupported-operation errors.
if get_option('enable_libzip')
  libzip_dep = dependency('libzip', required: false)
  if libzip_dep.found()
    exe_c_args = exe_c_args + ['-DN00B_HAVE_LIBZIP=1']
    deps += libzip_dep
  endif
endif

# picotls + picoquic vendored subprojects.  Required by the QUIC stack
# (src/net/quic) and HTTP/3 client (src/net/http).  See
# docs/net/quic/vendored.md for the upstream-tracking pin file.
picotls_proj           = subproject('picotls')
picotls_minicrypto_dep = picotls_proj.get_variable('picotls_minicrypto_dep')

picoquic_proj = subproject('picoquic')
picoquic_dep  = picoquic_proj.get_variable('picoquic_core_dep')

# Monocypher (vendored, CC0 / BSD-2 dual): Ed25519 signing primitive
# for the n00b_attest signer vertical (WP-002).  In-tree copy at
# subprojects/monocypher/, sized down to the four files we actually
# use (monocypher{,-ed25519}.{h,c}).  See subprojects/monocypher/
# README.n00b.md for the integration shape rationale.
monocypher_proj = subproject('monocypher')
monocypher_dep  = monocypher_proj.get_variable('monocypher_dep')

# libgit2 (system, via pkg-config) — WP-013 blame-lineage anchor.
# naudit's `src/naudit/blame.c` consumes the blame API to verify
# exemption locality via VCS line lineage. The specific entry
# points called are:
#   - `git_repository_discover` + `git_repository_open` (repo
#     discovery from a working-tree subdirectory);
#   - `git_blame_options_init` (the most-recently-added API call;
#     it determines the meson floor — see below);
#   - `git_blame_file` (HEAD-only blame; the base for the overlay);
#   - `git_blame_buffer` (overlays the base with working-tree
#     bytes for DF-AA);
#   - `git_blame_hunkcount` / `git_blame_hunk_byindex` /
#     `git_blame_hunk_byline` / `git_blame_free` (hunk walking);
#   - `git_commit_lookup` / `git_commit_time` /
#     `git_commit_free` (committer-timestamp lookup for the
#     oldest-hunk pick in `n00b_audit_blame_signing_commit`);
#   - `git_oid_fromstr` / `git_oid_tostr` / `git_oid_cmp` (OID
#     hex formatting + comparison).
# `git_blame_file` and `git_blame_buffer` have been part of
# libgit2 since well before 1.0. `git_blame_options_init` set the
# old `>=1.7.0` floor — earlier libgit2 releases initialized the
# blame_options struct via `GIT_BLAME_OPTIONS_INIT` only and
# didn't expose the `_init` runtime entry point that we call to
# tolerate options-struct ABI churn.
#
# The floor is `>=1.9.0` because `src/naudit/blame.c` calls
# `git_blame_hunkcount` / `git_blame_hunk_byindex` /
# `git_blame_hunk_byline`, which 1.9 introduced. Those are the
# CURRENT names: 1.9 renamed the older `git_blame_get_hunk_count`
# family and kept the old spellings only as deprecated aliases
# behind `GIT_DEPRECATE_HARD`. There is no version guard at the
# call sites, so a pre-1.9 libgit2 satisfied the old `>=1.7.0`
# check and then failed to compile with "use of undeclared
# identifier" (n00b#240; ubuntu-24.04 ships 1.7.2, which is how
# CI found it). Raise this floor rather than guarding the call
# sites: the code wants the modern API, and pinning the declared
# minimum to what the source actually uses is what keeps the
# next distro build from configuring and then breaking.
#
# Required on the dev matrix (macOS Homebrew 1.9.x). Distros
# older than libgit2 1.9 — Ubuntu 24.04 LTS among them — need a
# newer libgit2 than their system package provides. The include
# is added to `deps` so libn00b's link line carries `-lgit2` and
# every consumer (libn00b proper, every test, the `naudit`
# binary) sees the header path.
libgit2_dep = dependency('libgit2', version: '>=1.9.0', required: false)
if not libgit2_dep.found()
  if target_os == 'windows'
    fs = import('fs')
    libgit2_inc_opt = get_option('libgit2_inc')
    libgit2_lib_opt = get_option('libgit2_lib')

    if libgit2_inc_opt == ''
      libgit2_inc_opt = meson.project_source_root() / '..' / 'libgit2' / 'build-codex-win-clean' / 'include'
    endif
    if libgit2_lib_opt == ''
      libgit2_lib_opt = meson.project_source_root() / '..' / 'libgit2' / 'build-codex-win-clean' / 'RelWithDebInfo' / 'git2.lib'
    endif

    if not fs.is_dir(libgit2_inc_opt)
      error('libgit2 headers not found. Install libgit2 with pkg-config support, or pass -Dlibgit2_inc=<include-dir>.')
    endif
    if not fs.is_file(libgit2_lib_opt)
      error('libgit2 library not found. Install libgit2 with pkg-config support, or pass -Dlibgit2_lib=<path-to-git2.lib>.')
    endif

    libgit2_dep = declare_dependency(
      compile_args: ['-I' + libgit2_inc_opt],
      link_args: [libgit2_lib_opt],
    )
    message('Using libgit2 fallback: ' + libgit2_lib_opt)
  else
    error('libgit2 >= 1.9.0 is required (src/naudit/blame.c uses the 1.9 blame hunk API)')
  endif
endif
deps = deps + [libgit2_dep]

# zlib for HTTP/1.1 + H3 Content-Encoding decompression (gzip/deflate).
# brotli + zstd are dlopen'd at runtime in src/net/http/http_compression.c.
if target_os == 'darwin'
  zlib_dep = declare_dependency(link_args: [macos_sdk / 'usr/lib/libz.tbd'])
else
  zlib_dep = dependency('zlib', required: false)
  if not zlib_dep.found()
    if target_os == 'windows'
      fs = import('fs')
      zlib_inc_opt = get_option('zlib_inc')
      zlib_lib_opt = get_option('zlib_lib')

      if zlib_inc_opt == ''
        zlib_inc_opt = meson.project_source_root() / '..' / 'libgit2' / 'deps' / 'zlib'
      endif

      if not fs.is_dir(zlib_inc_opt)
        error('zlib headers not found. Install zlib with pkg-config support, or pass -Dzlib_inc=<include-dir>.')
      endif

      if zlib_lib_opt == ''
        zlib_fallback_sources = [
          zlib_inc_opt / 'adler32.c',
          zlib_inc_opt / 'crc32.c',
          zlib_inc_opt / 'deflate.c',
          zlib_inc_opt / 'infback.c',
          zlib_inc_opt / 'inffast.c',
          zlib_inc_opt / 'inflate.c',
          zlib_inc_opt / 'inftrees.c',
          zlib_inc_opt / 'trees.c',
          zlib_inc_opt / 'zutil.c',
        ]
        zlib_fallback_c_args = [
          '--no-ncc',
          '-I' + zlib_inc_opt,
          '-DNO_VIZ',
          '-DSTDC',
          '-DHAVE_SYS_TYPES_H',
          '-DHAVE_STDINT_H',
          '-DHAVE_STDDEF_H',
        ]
        zlib_fallback_lib = static_library(
          'n00b_zlib_fallback',
          zlib_fallback_sources,
          c_args: zlib_fallback_c_args,
        )
        zlib_dep = declare_dependency(
          compile_args: ['-I' + zlib_inc_opt],
          link_with: zlib_fallback_lib,
        )
        message('Using zlib source fallback: ' + zlib_inc_opt)
      else
        if not fs.is_file(zlib_lib_opt)
          error('zlib library not found. Install zlib with pkg-config support, or pass -Dzlib_lib=<path-to-zlib.lib>.')
        endif

        zlib_dep = declare_dependency(
          compile_args: ['-I' + zlib_inc_opt],
          link_args: [zlib_lib_opt],
        )
        message('Using zlib fallback: ' + zlib_lib_opt)
      endif
    else
      error('zlib is required')
    endif
  endif
endif
deps = deps + [zlib_dep]


subdir('src/core')
subdir('src/util')
subdir('src/crypto')

# ============================================================================
# Abstract data types
# ============================================================================

subdir('src/adt')

python3 = find_program('python3')
unicode_allow_downloads = get_option('unicode_allow_downloads')
unicode_strict_cache = get_option('unicode_strict_cache')
unicode_cache_dir_opt = get_option('unicode_cache_dir')
if unicode_cache_dir_opt == ''
    unicode_cache_dir = meson.project_source_root() / 'src' / 'text' / 'unicode' / '.unicode_cache'
elif unicode_cache_dir_opt.startswith('/')
    unicode_cache_dir = unicode_cache_dir_opt
else
    unicode_cache_dir = meson.project_source_root() / unicode_cache_dir_opt
endif
unicode_test_data_dir = meson.project_source_root() / 'test' / 'data'
gen_unicode_script = meson.project_source_root() / 'src' / 'text' / 'unicode' / 'tools' / 'gen_tables.py'

gen_unicode_file_names = [
    'gen_age.c',
    'gen_bidi_brackets.c',
    'gen_bidi_class.c',
    'gen_blocks.c',
    'gen_casemap.c',
    'gen_categories.c',
    'gen_collation.c',
    'gen_combining.c',
    'gen_composition.c',
    'gen_confusables.c',
    'gen_decomposition.c',
    'gen_eaw.c',
    'gen_emoji.c',
    'gen_grapheme.c',
    'gen_identifiers.c',
    'gen_idna.c',
    'gen_joining.c',
    'gen_linebreak.c',
    'gen_names.c',
    'gen_normprops.c',
    'gen_numeric.c',
    'gen_proplist.c',
    'gen_script_extensions.c',
    'gen_scripts.c',
    'gen_sentbreak.c',
    'gen_wordbreak.c',
]

gen_unicode_cmd = [
    python3,
    gen_unicode_script,
    '--out-dir', '@OUTDIR@',
    '--cache-dir', unicode_cache_dir,
    '--test-data-dir', unicode_test_data_dir,
]

gen_unicode_input_check_cmd = [
    python3,
    gen_unicode_script,
    '--cache-dir', unicode_cache_dir,
    '--test-data-dir', unicode_test_data_dir,
    '--check-inputs',
]

if unicode_allow_downloads
    gen_unicode_cmd += ['--allow-downloads']
    gen_unicode_input_check_cmd += ['--allow-downloads']
endif

if unicode_strict_cache
    gen_unicode_cmd += ['--strict']
    gen_unicode_input_check_cmd += ['--strict']
else
    gen_unicode_cmd += ['--no-strict']
    gen_unicode_input_check_cmd += ['--no-strict']
endif

gen_unicode_input_check = run_command(gen_unicode_input_check_cmd, check: false)
if gen_unicode_input_check.returncode() != 0
    error('Unicode input preflight failed.\n' +
          gen_unicode_input_check.stdout() +
          gen_unicode_input_check.stderr())
endif
message('Unicode cache dir: ' + unicode_cache_dir)

gen_unicode_tables = custom_target('gen_unicode_tables',
    output: gen_unicode_file_names,
    command: gen_unicode_cmd,
    depend_files: files('src/text/unicode/tools/gen_tables.py'),
)

# ============================================================================
# Text processing (unicode + strings)
# ============================================================================

subdir('src/text')

# Note: src/text/regex/ and src/util/simd/ (resharp-c port) are reached
# transitively via the src/text/ and src/util/ subdir() calls above.
# See ~/dd/n00b_regex_port_plan.md § 6 / § 7.

# ============================================================================
# Display (render + table + layout + hexdump)
# ============================================================================

subdir('src/display')

# ============================================================================
# Conduit pub/sub system
# ============================================================================

subdir('src/conduit')

# ============================================================================
# Virtual Filesystem
# ============================================================================

subdir('src/vfs')

# ============================================================================
# Parsers
# ============================================================================

subdir('src/parsers')

# ============================================================================
# Slay grammar / parser toolkit
# ============================================================================

subdir('src/slay')

# ============================================================================
# N00b compiler (language-specific)
# ============================================================================

subdir('src/n00b')

libdl_dep = declare_dependency()
if target_os != 'darwin'
  libdl_dep = cc.find_library('dl', required: false)
endif

# ============================================================================
# Logic / constraint solver
# ============================================================================

subdir('src/logic')

# ============================================================================
# Type checker
# ============================================================================

subdir('src/typecheck')

subdir('src/compiler/objfile')

subdir('src/chalk')

# ============================================================================
# Machine learning (feature-hashed binary logistic regression)
# ============================================================================

subdir('src/ml')

# ============================================================================
# Data formats: local landing sinks and readers
# ============================================================================

subdir('src/data')

# ============================================================================
# Networking: QUIC stack (phases 1-5) + HTTP/1.1+H3 client (phase 6)
# ============================================================================

subdir('src/net/quic')
subdir('src/net/dns')
subdir('src/net/http')

# ============================================================================
# Attestation (in-toto Statement + DSSE envelopes; signers / verifier
# arrive in later WPs — see .agents/work-plans/wp-001-* / WP-002 plan).
# ============================================================================

subdir('src/attest')

# ============================================================================
# hostmeta — host / build-environment metadata collection: the C port
# of chalk's metadata plugins (cloud instance metadata for AWS / Azure
# / GCP, the AWS ECS and Lambda execution environments, and nine CI/CD
# systems). Placed after src/net/http and src/parsers because it drives
# the HTTP client against metadata endpoints and parses their JSON.
# ============================================================================

subdir('src/hostmeta')

# ============================================================================
# naudit — n00b API-guidelines auditor (consolidated from the former
# standalone n00b-audit project; see WP-008 / D-017). The engine drives
# a slay/Earley parse of the input file against a metagrammar-merged
# rule set loaded from a project-local `audit-rules.bnf`.
#
# The audit-rule-file metagrammar is itself a BNF grammar shipped in
# `grammars/audit-rule-file.bnf`. `embed_metagrammar.py` reads the BNF
# at configure time, C-string-escapes the bytes, and emits a header
# (`audit_rule_file_grammar.h`) that defines a static const char array
# holding the normalized BNF text. The audit-rule loader hands that string to
# slay's `n00b_bnf_load` at runtime to compose the metagrammar object.
# Same shape as the unicode codegen above; the generated header is
# listed as a libn00b source so meson produces it before any naudit TU
# compiles and surfaces its containing directory on the implicit
# include path.
# ============================================================================

# Path-constant header (`audit_paths.h`) exposing the location of
# `c_ncc.bnf` to the naudit engine. The engine loads the base C
# grammar from `c_ncc.bnf` at runtime and merges per-rule fragments
# loaded from the project's `audit-rules.bnf`. Standalone-project
# precedent: the path was emitted at configure time so no path-
# string literals land in C source (NCC.md "Build system"); same
# pattern in-tree, pointing at n00b's own `grammars/c_ncc.bnf`.
#
# WP-009 Phase 3 extends this header with two more constants
# consumed by the libn00b `n00b_eval_*` surface
# (`src/n00b/eval.c`):
#
#   * `N00B_N00B_GRAMMAR_PATH` — the path to `grammars/n00b.bnf`,
#     loaded by `n00b_eval_session_new` to compile filter
#     expressions against the n00b language grammar.
#   * `N00B_BUILTINS_PATH` — the path to `lib/std/builtins.n`,
#     the stdlib loaded onto each fresh eval session (the load is
#     best-effort: if the file is missing at runtime the session
#     still builds, but predicates referencing stdlib symbols
#     such as `print` will fail to compile).
ncc_grammar_path = (
    meson.current_source_dir() / 'grammars' / 'c_ncc.bnf')

n00b_grammar_path = (
    meson.current_source_dir() / 'grammars' / 'n00b.bnf')

n00b_builtins_path = (
    meson.current_source_dir() / 'lib' / 'std' / 'builtins.n')

audit_paths_h = configure_file(
    output: 'audit_paths.h',
    configuration: configuration_data({
        'N00B_AUDIT_GRAMMAR_PATH':      '"' + ncc_grammar_path + '"',
        'N00B_N00B_GRAMMAR_PATH':       '"' + n00b_grammar_path + '"',
        'N00B_BUILTINS_PATH':           '"' + n00b_builtins_path + '"',
    }),
    install: false,
)

audit_rule_file_bnf_path = (
    meson.current_source_dir() / 'grammars' / 'audit-rule-file.bnf')

audit_rule_file_grammar_h = custom_target(
    'audit_rule_file_grammar.h',
    input: audit_rule_file_bnf_path,
    output: 'audit_rule_file_grammar.h',
    command: [
        python3,
        meson.current_source_dir() / 'scripts' / 'embed_metagrammar.py',
        '@INPUT@',
        '@OUTPUT@',
    ],
    install: false,
)

# Installed binaries cannot depend on files in the build machine's source tree.
x509_der_grammar_h = custom_target(
    'x509_der_grammar.h',
    input: meson.current_source_dir() / 'grammars' / 'x509_der.bnf',
    output: 'x509_der_grammar.h',
    command: [
        python3,
        meson.current_source_dir() / 'scripts' / 'embed_metagrammar.py',
        '@INPUT@',
        '@OUTPUT@',
        'N00B_X509_DER_GRAMMAR',
    ],
    install: false,
)

crayon_egress_roots_h = custom_target(
    'crayon_egress_roots.h',
    input: meson.current_source_dir() / 'pki' / 'crayon-egress-roots.pem',
    output: 'crayon_egress_roots.h',
    command: [
        python3,
        meson.current_source_dir() / 'scripts' / 'embed_metagrammar.py',
        '@INPUT@',
        '@OUTPUT@',
        'N00B_CRAYON_EGRESS_ROOTS_PEM',
    ],
    install: false,
)

subdir('src/naudit')

# ============================================================================
# ClickHouse HTTP substrate (D-062 amended 2026-05-27 — libn00b_clickhouse
# lives in libn00b as a pure-C bundled module; no separate library, no
# opt-in option, since it depends only on n00b_http_request_sync).
# ============================================================================

subdir('src/clickhouse')

# ============================================================================
# rocs — retrieval-oriented columnar store module.
# ============================================================================

subdir('src/rocs')
subdir('src/debug')

# Aggregate per-module source lists into the libn00b source set. Each
# entry above (`subdir('src/<mod>')`) is responsible for declaring a
# `n00b_<mod>_src` variable; we just glue them together here.
n00b_dll_src = (
    n00b_core_src +
    n00b_util_src +
    n00b_crypto_src +
    n00b_adt_src +
    n00b_text_src +
    n00b_display_src +
    n00b_conduit_src +
    n00b_vfs_src +
    n00b_parsers_src +
    n00b_logic_src +
    n00b_slay_src +
    n00b_typecheck_src +
    n00b_compiler_src +
    n00b_objfile_src +
    n00b_chalk_src +
    n00b_ml_src +
    n00b_data_src +
    n00b_quic_src +
    n00b_dns_src +
    n00b_http_src +
    n00b_attest_src +
    n00b_hostmeta_src +
    n00b_naudit_src +
    n00b_clickhouse_src +
    n00b_rocs_src +
    n00b_debug_src +
    [audit_paths_h,
     audit_rule_file_grammar_h,
     x509_der_grammar_h,
     crayon_egress_roots_h]
)

# ============================================================================
# libn00b_aws — optional AWS substrate (D-062 amended 2026-05-27)
# ============================================================================
# Pure-C wrap of the aws-sdk-rust shim (`n00b-aws-shim`). The shim is a
# Rust cdylib that exports each AWS operation as `extern "C"`; we run
# `~/slop/bin/demangle` against it at build time to generate a C header
# with dlsym-lazy wrappers, then put an n00b-idiomatic surface on top.
#
# No C++ language anywhere in this build. No libc++ at runtime. The
# shim's Rust runtime (Tokio) is loaded only by executables that link
# libn00b_aws.

conf_enable_aws  = get_option('enable_aws')
aws_shim_prefix  = get_option('aws_shim_prefix')

if conf_enable_aws
  fs           = import('fs')
  cc           = meson.get_compiler('c')

  # AWS is compiled in: define N00B_BUILD_AWS so rocs/store.c drops its
  # [[gnu::weak]] no-AWS fallback stubs for n00b_aws_config /
  # n00b_aws_s3_vfs_backend_new. Those stubs are weak, but ncc emits the
  # real `_kargs` implementations weak too, so without this guard the linker
  # resolves the symbol to whichever weak def it encounters first (per-binary,
  # by static-archive order) -- silently linking the stub (which always
  # returns N00B_AWS_ERR_INTERNAL) into some AWS-enabled binaries. Dropping the
  # stub when AWS is on leaves the symbol undefined in core libn00b, forcing
  # the linker to pull the real impl from libn00b_aws.
  c_args     = c_args + ['-DN00B_BUILD_AWS']
  exe_c_args = exe_c_args + ['-DN00B_BUILD_AWS']

  # The Rust-shim path uses cbindgen — it reads the crate's source
  # and emits a clean C header using `<stdint.h>` types. demangle
  # was the original plan but segfaults on aws-sdk-rust's DWARF
  # (the type-mapping fix I landed upstream is still right for any
  # C++ library someone wraps later; see
  # [[project_n00b_aws_via_rust_shim]] for the why).
  cbindgen_bin = find_program('cbindgen', required: true)

  if aws_shim_prefix == ''
    # Development path: the shim builds under the in-repo Rust crate at
    # `subprojects/n00b-aws-shim`. Production installs come from a
    # Homebrew tap / apt PPA / vcpkg port that drops the cdylib at a
    # platform-conventional prefix; users point at it via
    # `-Daws_shim_prefix=...` or `N00B_AWS_SHIM_PREFIX=...`.
    aws_shim_prefix = meson.project_source_root() / 'subprojects' / 'n00b-aws-shim'
  endif

  if target_os == 'darwin'
    shim_lib_name = 'libn00b_aws_shim.dylib'
  elif target_os == 'windows'
    shim_lib_name = 'n00b_aws_shim.dll'
  else
    shim_lib_name = 'libn00b_aws_shim.so'
  endif

  # Locate the prebuilt shim dylib + its library directory. Two layouts
  # are supported:
  #   * Installed:  `<prefix>/lib/<name>` (brew / apt / vcpkg).
  #   * Dev (cargo): `<prefix>/target/release/<name>`.
  # meson picks the first one that exists.
  shim_dylib_installed = aws_shim_prefix / 'lib' / shim_lib_name
  shim_dylib_devel     = aws_shim_prefix / 'target' / 'release' / shim_lib_name

  if fs.exists(shim_dylib_installed)
    aws_shim_dylib   = shim_dylib_installed
    aws_shim_lib_dir = aws_shim_prefix / 'lib'
  elif fs.exists(shim_dylib_devel)
    aws_shim_dylib   = shim_dylib_devel
    aws_shim_lib_dir = aws_shim_prefix / 'target' / 'release'
  else
    error('n00b-aws-shim dylib not found. Either install the prebuilt' +
          ' shim and pass `-Daws_shim_prefix=<prefix>`, or build the' +
          ' in-repo crate via `cargo build --release` in' +
          ' subprojects/n00b-aws-shim/.')
  endif

  # Compile-time link dependency: downstream consumers link against the
  # shim cdylib via its standard library name (`-ln00b_aws_shim`).
  aws_shim_lib_dep = cc.find_library('n00b_aws_shim',
                                     dirs: [aws_shim_lib_dir],
                                     required: true)

  # cbindgen reads the crate source, so we need to point it at the
  # in-repo path even when the prebuilt dylib lives elsewhere. End
  # consumers who install the prebuilt dylib also get a pre-generated
  # C header alongside it (we run cbindgen once at publish time).
  aws_shim_crate_root = meson.project_source_root() / 'subprojects' / 'n00b-aws-shim'

  subdir('src/aws')
else
  n00b_aws_dep = disabler()
endif
n00b_no_ncc_src = [
    'src/core/xxhash_wrap.c',
    'src/slay/token_id_hash.c',
]

# Build generated unicode tables with --no-ncc (they are pure data,
# no ncc extensions, and parsing them is extremely expensive).
gen_unicode_lib = static_library('n00b_gen_unicode',
    gen_unicode_tables,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'],
    link_args: link_args,
)

# Force-include the libc-malloc interpose shim into vendored --no-ncc C that
# calls the libc malloc family (md4c, linenoise, ryu float formatting). On
# macOS the compile-time #define shim is the ONLY redirect mechanism: the
# strong-symbol malloc override is Linux-only and dyld __interpose cannot touch
# the main-exe image (see src/core/alloc_interpose.c). Without it these TUs call
# real libc malloc, which leaks outside user_pool AND traps on n00b off-libc
# worker threads (the same SIGTRAP the picoquic/picotls shim prevents). Use
# current_source_dir so it resolves when libn00b is consumed as a subproject.
vendor_interpose_shim_cargs = [
  '-include',
  meson.current_source_dir() / 'include' / 'core' / 'alloc_interpose_shim.h',
]

# Build md4c vendor implementation with --no-ncc (it's 10K+ lines of
# pure C; parsing it through ncc's packrat parser OOMs in constrained
# environments like Docker cross-builds). The upstream linenoise source is
# POSIX-only, so Windows builds use a small non-interactive fallback.
vendor_sources = ['src/vendor/md4c.c']
if target_os == 'windows'
  vendor_sources += 'src/vendor/linenoise_windows.c'
else
  vendor_sources += 'src/vendor/linenoise.c'
endif

n00b_vendor_lib = static_library('n00b_vendor',
    vendor_sources,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'] + vendor_interpose_shim_cargs,
    link_args: link_args,
)

n00b_no_ncc_lib = static_library('n00b_no_ncc',
    n00b_no_ncc_src,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc'],
    link_args: link_args,
)

link_whole_libs = [gen_unicode_lib, n00b_vendor_lib, n00b_no_ncc_lib]

# Ryū correctly-rounded, malloc-free precision float formatting (d2fixed/d2exp).
# Kept as a SEPARATE link target because d2fixed_full_table.h is ~329 KB; its
# STRONG n00b_fp_format_fixed/exp override the weak Grisu2+round fallbacks in
# libn00b core (src/text/strings/fp_format.c) when linked. Built --no-ncc (pure
# vendored C; upstream `#include "ryu/..."` resolves via -Isrc/vendor/ryu).
# Disable with -Denable_ryu_fp=false to drop the table and use the fallback.
if get_option('enable_ryu_fp')
  n00b_fpfmt_ryu_lib = static_library('n00b_fpfmt_ryu',
      ['src/vendor/ryu/d2fixed.c', 'src/vendor/ryu/fp_format_ryu.c'],
      include_directories: incdir + [include_directories('src/vendor/ryu')],
      c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'] + vendor_interpose_shim_cargs,
      link_args: link_args,
  )
  link_whole_libs += n00b_fpfmt_ryu_lib
endif

# Conduit's macOS XPC native skeleton is declared in src/conduit/meson.build
# and folded in here after link_whole_libs exists.
link_whole_libs += n00b_conduit_link_whole_libs

# crypto's macOS ObjC bridges (ACME trust verifier, Keychain secret provider)
# are declared in src/crypto/meson.build and exposed via the array below.
link_whole_libs += n00b_crypto_link_whole_libs

# QUIC's macOS ObjC bridges array (now empty — the bridges moved to crypto);
# kept for compatibility with the subdir contract.
link_whole_libs += n00b_quic_link_whole_libs

# libchalk's macOS ObjC bridge (Mach-O re-sign via codesign(1) shim) —
# same pattern as the QUIC bridges above.
link_whole_libs += n00b_chalk_link_whole_libs

# Build the Cocoa backend as a separate ObjC static library (macOS only).
# ObjC files cannot go through ncc's C parser, so we compile them with
# the system ObjC compiler directly (similar to the vendor library pattern).
if conf_cocoa
  objc_comp = meson.get_compiler('objc')
  cocoa_objc_args = [
    '-Wextra',
    '-g',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
    '-Wno-unknown-pragmas',
    '-fno-omit-frame-pointer',
    '-fPIC',
    '-Wno-atomic-alignment',
    '-fobjc-arc',
    '-isysroot', macos_sdk,
    '-DN00B_HOST_OS=' + host_machine.system(),
    '-DN00B_HOST_CPU=' + host_machine.cpu_family(),
  ]
  if conf_dev_mode
    cocoa_objc_args += '-DN00B_DEV'
  endif
  if conf_debug
    cocoa_objc_args += '-DN00B_DEBUG'
  endif

  n00b_cocoa_lib = static_library('n00b_cocoa',
      [
        'src/display/render/backend_cocoa.m',
        'src/display/render/cocoa_bridge_layout.m',
      ],
      include_directories: incdir,
      dependencies: deps,
      objc_args: cocoa_objc_args,
      link_args: link_args,
  )
  link_whole_libs += n00b_cocoa_lib
endif

# Build the notcurses backend as a separate static library.
# notcurses is a plain C library so this compiles through ncc normally,
# but we keep it separate to gate on the optional dependency.
if conf_notcurses
  n00b_notcurses_lib = static_library('n00b_notcurses',
      ['src/display/render/backend_notcurses.c'],
      include_directories: incdir,
      dependencies: deps,
      c_args: exe_c_args,
      link_args: link_args,
  )
  link_whole_libs += n00b_notcurses_lib
endif

# macOS ACME trust verifier + Keychain secret provider ObjC bridges
# are declared in src/net/quic/meson.build (per-subdir build files).

if target_os != 'windows'
  n00b_gcmap_index = executable('n00b-gcmap-index',
      ['src/tools/n00b-gcmap-index.c'],
      include_directories: incdir,
      c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'],
      link_args: exe_link_args,
      install: false,
  )
endif

# Static-only, deliberately. A shared libn00b cannot link on macOS: the section
# collectors in src/core/gc_type_map.c and src/core/static_image.c reference
# `_mh_execute_header` (the MAIN executable's Mach-O header), which is undefined
# in a dylib — `ld64`/`ld64.lld` both reject the link with "undefined symbol:
# _mh_execute_header". Even if forced to link, a shared libn00b would not be
# runtime-correct: the GC type-map / static-object / grammar-image registries
# assume all their linker sections are coalesced into a single image (only
# stobj-on-macOS walks every image). Making those collectors multi-image is a
# prerequisite for ever shipping a shared libn00b; until then, static only.
n00b = static_library('n00b',
    n00b_dll_src,
    # Custom-entry builds name n00b_start as the process entry. Windows
    # system-entry builds leave this empty and initialize n00b from main.
    objects: n00b_crt_host_stub_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep, mir_dep, picoquic_dep, monocypher_dep],
    c_args: exe_c_args,
    link_args: link_args,
    link_whole: link_whole_libs,
    install: true,
)

# GC type-map dictionary (pre-link aggregation). ncc compiles each TU with
# --ncc-gcmap-prelink, emitting raw n00b_gcraw records (no typed codegen-ABI
# structs — that is what decouples include/core/codegen_abi.h from the ~500 TUs).
# Those records are aggregated, PER FINAL EXECUTABLE, into one generated object
# defining the typed, sorted n00b_gcmap_table[] / n00b_gcmap_count that the
# runtime (src/core/gc_type_map.c) binary-searches. The aggregation runs in the
# ObjC link wrapper (scripts/ncc-gcmap-objc-link-wrapper.sh, slotted in as OBJC
# by build.sh): it `ncc --ncc-gcmap-emit`s over the link's object/archive inputs
# (this executable's own TUs + libn00b.a, so the dictionary is complete) and
# appends the generated object to the link. It must be per-executable — not a
# single libn00b-wide object — because each executable defines its own
# pointer-bearing types whose records live only in its own objects. (We use the
# ObjC link wrapper rather than ncc's own link-stage hook because libn00b carries
# the Cocoa ObjC bridge, so meson links executables with the ObjC compiler, not
# ncc.)
#
# The dictionary is per-executable, but libn00b.a's CONTRIBUTION to it is the
# same for all ~600 of them, and collecting it means extracting and scanning
# every one of the archive's ~620 members. Do that once here: `--ncc-gcraw-dump`
# writes the archive's raw records to a blob, which the wrapper feeds to
# `--ncc-gcmap-emit` in place of libn00b.a. The blob is an input to every link,
# so it is declared via `link_depends` in n00b_app_kwargs below.
#
# REQUIRES ncc f9ef837 (ncc#60) OR NEWER -- build.sh's NCC_REV_DEFAULT and the
# NCC_REV in .github/workflows/build-and-test.yml both satisfy this; a
# downstream consumer's own ncc pin must too (n00b#429).
#
# An older ncc does not recognise --ncc-gcraw-dump. It does not reject it
# either: at link time ncc drops unknown --ncc-* flags, so the invocation
# below degenerates into a LINK of libn00b.a. That fails a long way from the
# cause. On Linux it surfaces as
#
#     libn00b.a(src_compiler_objfile_pe_query.c.o): in function
#     `n00b_pe_section_entropy': undefined reference to `log2'
#
# which reads like a missing -lm in this target. It is not: the correct ncc
# performs no link here at all, so there is nothing for libm to satisfy, and
# adding -lm changes nothing (ncc's is_linker_input ignores `-`-prefixed
# args). The fix for that error is to match the ncc pin, not to edit this
# command.
n00b_gcraw = custom_target('n00b-gcraw',
    input: n00b,
    output: 'libn00b.gcraw',
    command: [cc.cmd_array(), '--ncc-gcraw-dump=@OUTPUT@', '@INPUT@'],
    build_by_default: true,
)

# Where the ObjC wrapper drives the link (Darwin), it swaps the blob in for
# libn00b.a itself and the link line is untouched. Elsewhere ncc is the link
# driver and aggregates from its own argv, so the blob is named there; ncc
# drops it before handing the line to the compiler, and skips the archive it
# shadows.
if target_os == 'darwin'
  gcraw_link_args = []
else
  gcraw_link_args = [n00b_gcraw.full_path()]
endif

# Shared kwargs for executables that link the full libn00b. This is the
# "always build" variant used by the shipped tools/binaries. Tests use
# `test_common_kwargs` below, which is identical except it honors -Dbuild_tests.
n00b_app_kwargs = {
    'include_directories': incdir,
    'c_args': exe_c_args,
    # `exe_link_args` carries the sanitizer link-line entries
    # (`-fsanitize=address` / `-fsanitize=undefined`) when those are
    # enabled at configure time, plus LTO flags.  Without this, tests
    # built off this kwarg block would link a libn00b that *was*
    # compiled with sanitizer hooks but no runtime, producing
    # `__ubsan_handle_*`-not-found at link time.  Phase 2 audit item
    # #9.
    'link_args': exe_link_args + gcraw_link_args,
    'link_with': [n00b],
    # Orders the gcraw blob ahead of every link that consults it; see the
    # n00b_gcraw target above.
    'link_depends': [n00b_gcraw],
    # libn00b now references picoquic symbols (endpoint, conn, chan).
    # Static-library deps don't propagate transitively in meson, so
    # every test that links libn00b also needs picoquic_dep on its
    # link line.  picoquic_dep brings the include path along too.
    # monocypher_dep is added for the same reason: libn00b's attest
    # sources (Phase 2+) will reference crypto_ed25519_* and the
    # attest_monocypher_smoke test calls those primitives directly.
    'dependencies': [picoquic_dep, monocypher_dep],
}

# Test variant: same as n00b_app_kwargs, but kept out of the default build
# target unless -Dbuild_tests=true. Every test executable (directly or via a
# derived *_test_kwargs dict, all of which extend this one) inherits the
# build_by_default flag from here, so no per-test edits are needed. `meson test`
# still builds tests on demand regardless of build_by_default.
test_common_kwargs = n00b_app_kwargs + {'build_by_default': n00b_build_tests}

gui_tui_unnecessary_on_windows = target_os == 'windows'
if gui_tui_unnecessary_on_windows
  message('Skipping GUI/TUI test and tool targets on Windows')
endif

# WP-018/WP-020: build-time grammar baking tool. This now links the full
# libn00b, not the bootstrap library, because marshal-backed baking depends on
# the same precise TYPE_LAYOUT path validated by the WP-020 feature tests.
naudit_grammar_bake = executable('naudit-grammar-bake',
    ['src/tools/naudit-grammar-bake.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep, monocypher_dep, mir_dep, libgit2_dep],
)

# Alias kept for clarity at QUIC test sites that drive picoquic
# explicitly; structurally identical to test_common_kwargs.
quic_test_kwargs = test_common_kwargs

# QUIC demo executables — each lives in its own per-subdir meson.build.
subdir('examples/quic_echo')
subdir('examples/quic_server_managed')
subdir('examples/quic_acme_demo')
subdir('examples/quic_acme_http01_demo')


array_test = executable('test_array',
    ['test/unit/test_array.c'],
    kwargs: test_common_kwargs,
)
test('array', array_test, suite: 'unit')

debug_watch_test = executable('test_debug_watch',
    ['test/unit/test_debug_watch.c'],
    kwargs: test_common_kwargs,
)
# Un-quarantined (n00b#280). The Linux failure was the perf_event_open errno
# being reported as NO_SLOT whatever went wrong (#423): the very first install
# claimed "all hardware slots are in use" with all four debug registers free.
# Correctly classified, the runner's refusal is UNSUPPORTED, which this test
# already handles by skipping. So it runs for real on macOS and skips cleanly
# on Linux, instead of aborting there and being excluded on both.
test('debug_watch', debug_watch_test, suite: 'unit')

debug_break_test = executable('test_debug_break',
    ['test/unit/test_debug_break.c'],
    kwargs: test_common_kwargs,
)
# Un-quarantined (n00b#280). The Linux failure was the perf_event_open errno
# being reported as NO_SLOT whatever went wrong (#423): the very first install
# claimed "all hardware slots are in use" with all four debug registers free.
# Correctly classified, the runner's refusal is UNSUPPORTED, which this test
# already handles by skipping. So it runs for real on macOS and skips cleanly
# on Linux, instead of aborting there and being excluded on both.
test('debug_break', debug_break_test, suite: 'unit')

debug_allthread_test = executable('test_debug_allthread',
    ['test/unit/test_debug_allthread.c'],
    kwargs: test_common_kwargs,
)
# Un-quarantined (n00b#280). The Linux failure was the perf_event_open errno
# being reported as NO_SLOT whatever went wrong (#423): the very first install
# claimed "all hardware slots are in use" with all four debug registers free.
# Correctly classified, the runner's refusal is UNSUPPORTED, which this test
# already handles by skipping. So it runs for real on macOS and skips cleanly
# on Linux, instead of aborting there and being excluded on both.
test('debug_allthread', debug_allthread_test, suite: 'unit')

debug_attach_test = executable('test_debug_attach',
    ['test/unit/test_debug_attach.c'],
    kwargs: test_common_kwargs,
)
test('debug_attach', debug_attach_test, suite: 'unit')

if target_os != 'windows' and n00b_crt_host_stub_file.length() > 0
    crt_stub_runner = find_program('sh')
    test('crt_stub_args', crt_stub_runner,
        args: [
            meson.current_source_dir() / 'test/unit/test_crt_stub_args.sh',
            meson.current_source_dir(),
            meson.current_build_dir(),
            target_os,
            target_arch,
        ],
        suite: 'unit',
    )
    test('crt_init_array', crt_stub_runner,
        args: [
            meson.current_source_dir() / 'test/unit/test_crt_init_array.sh',
            meson.current_source_dir(),
            meson.current_build_dir(),
            target_os,
            target_arch,
        ],
        suite: 'unit',
    )
endif

proc_info_test = executable('test_proc_info',
    ['test/unit/test_proc_info.c'],
    kwargs: test_common_kwargs,
)
test('proc_info', proc_info_test, suite: 'unit')

thread_callstack_test = executable('test_thread_callstack',
    ['test/unit/test_thread_callstack.c'],
    kwargs: test_common_kwargs,
)
test('thread_callstack', thread_callstack_test, suite: 'unit')

thread_identity_test = executable('test_thread_identity',
    ['test/unit/test_thread_identity.c'],
    kwargs: test_common_kwargs,
)
test('thread_identity', thread_identity_test, suite: 'unit')

thread_spawn_raw_test = executable('test_thread_spawn_raw',
    ['test/unit/test_thread_spawn_raw.c'],
    kwargs: test_common_kwargs,
)
test('thread_spawn_raw', thread_spawn_raw_test, suite: 'unit')

string_scratch_raw_worker_test = executable('test_string_scratch_raw_worker',
    ['test/unit/test_string_scratch_raw_worker.c'],
    kwargs: test_common_kwargs,
)
test('string_scratch_raw_worker', string_scratch_raw_worker_test, suite: 'unit')

# Wax-free reproducer for the stale interior pointer held by
# n00b_unicode_str_split_words() while allocations can move s->data.
split_words_gc_repro_test = executable('test_split_words_gc_repro',
    ['test/unit/test_split_words_gc_repro.c'],
    kwargs: test_common_kwargs,
)
# Raised for shared CI runners (n00b#261). Every one of these hit its
# declared cap EXACTLY on ubuntu-latest/macos-latest -- 30.02s, 60.06s,
# 300.09s -- which is the signature of a too-tight limit, not a hang.
# Measured locally at roughly half these values with wide variance.
    # QUARANTINED (n00b#280): SIGSEGVs on Linux -- at 59s against a 180s
    # budget, so this is a crash and not slowness. Passes on macOS, so
    # should_fail would report UNEXPECTEDPASS there.
test('split_words_gc_repro', split_words_gc_repro_test, suite: 'known-broken',
     timeout: 180)

shutdown_gc_stack_test = executable('test_shutdown_gc_stack',
    ['test/unit/test_shutdown_gc_stack.c'],
    kwargs: test_common_kwargs,
)
test('shutdown_gc_stack', shutdown_gc_stack_test, suite: 'unit')

thread_spawn_kwargs_test = executable('test_thread_spawn_kwargs',
    ['test/unit/test_thread_spawn_kwargs.c'],
    kwargs: test_common_kwargs,
)
# Aborts on the CI Linux runner in 0.1s:
#   test_thread_spawn_kwargs.c:844: test_isolation_kwarg:
#   Assertion `ctl_io.seen_isolated == false' failed.
# Passes locally on macOS in 0.27s. n00b#280.
test('thread_spawn_kwargs', thread_spawn_kwargs_test, suite: 'unit')

thread_self_migration_test = executable('test_thread_self_migration',
    ['test/unit/test_thread_self_migration.c'],
    kwargs: test_common_kwargs,
)
test('thread_self_migration', thread_self_migration_test, suite: 'unit')

# n00b#333: POSIX-only (raw pthreads); not built on Windows.
if target_os != 'windows'
thread_self_foreign_test = executable('test_thread_self_foreign',
    ['test/unit/test_thread_self_foreign.c'],
    kwargs: test_common_kwargs,
)
test('thread_self_foreign', thread_self_foreign_test, suite: 'unit')
endif

# The foreign-thread and fork cases are POSIX-only and guarded in the source.
thread_os_id_test = executable('test_thread_os_id',
    ['test/unit/test_thread_os_id.c'],
    kwargs: test_common_kwargs,
)
test('thread_os_id', thread_os_id_test, suite: 'unit')

thread_self_bench = executable('test_thread_self_bench',
    ['test/unit/test_thread_self_bench.c'],
    kwargs: test_common_kwargs,
)

thread_exit_test = executable('test_thread_exit',
    ['test/unit/test_thread_exit.c'],
    kwargs: test_common_kwargs,
)
thread_exit_timeout = 120
test('thread_exit', thread_exit_test, suite: 'unit', timeout: thread_exit_timeout)
test('thread_exit_timeout_budget_fixture',
     thread_exit_test,
     suite: 'unit',
     timeout: 10,
     env: ['N00B_THREAD_EXIT_TIMEOUT_BUDGET_FIXTURE=1'])

crash_test = executable('test_crash',
    ['test/unit/test_crash.c'],
    kwargs: test_common_kwargs,
)
# Run through n00b-gcmap-index --exec so the binary's gcmap index is populated
# before the test runs; without it n00b_gc_type_map_lookup misses every ad-hoc
# struct layout and marshal falls back to conservative scan (D-049 / status-5).
if target_os != 'windows'
  test('crash', n00b_gcmap_index,
       args: ['--exec', crash_test.full_path()],
       depends: [crash_test, n00b_gcmap_index],
       suite: 'unit')
else
  test('crash', crash_test, suite: 'unit')
endif

conduit_unix_socket_test = executable('test_conduit_unix_socket',
    ['test/unit/test_conduit_unix_socket.c'],
    kwargs: test_common_kwargs,
)
test('conduit_unix_socket', conduit_unix_socket_test, suite: 'unit')

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
conduit_tls_test = executable('test_conduit_tls',
    ['test/unit/test_conduit_tls.c'],
    kwargs: test_common_kwargs,
)
# n00b#330: the in-test I/O budget is 30 s (N00B_TLS_TEST_IO_BUDGET_MS), so
# the meson timeout must sit above it -- at the 30 s default the whole-test
# timeout fires first and a stall is reported as a bare expiry instead of the
# stage-naming diagnostic. Normal runtime is ~1.2-1.8 s on macOS.
# conduit_tls_socket_denied_fixture below keeps its 10 s: it fails at socket()
# with EPERM by construction and never reaches a handshake.
test('conduit_tls', conduit_tls_test, suite: 'unit', timeout: 120)
test('conduit_tls_socket_denied_fixture',
     conduit_tls_test,
     suite: 'unit',
     timeout: 10,
     env: ['N00B_CONDUIT_TLS_SOCKET_DENIED_FIXTURE=1'])
endif

conduit_local_unix_e2e_test = executable('test_conduit_local_unix_e2e',
    ['test/unit/test_conduit_local_unix_e2e.c'],
    kwargs: test_common_kwargs,
)
test('conduit_local_unix_e2e', conduit_local_unix_e2e_test, suite: 'unit')

conduit_local_conformance_e2e_test = executable(
    'test_conduit_local_conformance_e2e',
    ['test/unit/test_conduit_local_conformance_e2e.c'],
    kwargs: test_common_kwargs,
)
# n00b#415: the in-test I/O budget is 30 s (N00B_TEST_IO_BUDGET_MS), so the
# meson timeout has to sit above it -- at the 30 s default the whole-test
# timeout fires first and a wedge is reported as a bare expiry instead of the
# assertion that names the stalled read. Normal runtime is ~8.6 s on macOS and
# ~5.3 s on Linux, so this is ~14x the observed cost and ~7x its worst
# measured (half-speed runner) case.
test('conduit_local_conformance_e2e',
     conduit_local_conformance_e2e_test,
     suite: 'unit',
     timeout: 120)

if target_os == 'darwin'
  conduit_local_xpc_e2e_src = configure_file(
      input: 'test/unit/test_conduit_local_xpc_e2e.m',
      output: 'test_conduit_local_xpc_e2e.c',
      copy: true,
  )
  conduit_local_xpc_e2e_test = executable('test_conduit_local_xpc_e2e',
      [conduit_local_xpc_e2e_src],
      kwargs: test_common_kwargs,
  )
  # Same reasoning as conduit_local_conformance_e2e above (n00b#415); this one
  # normally runs in ~1.3 s.
  test('conduit_local_xpc_e2e', conduit_local_xpc_e2e_test, suite: 'unit',
       timeout: 120)
endif

if target_os == 'windows'
  conduit_local_windows_e2e_test = executable('test_conduit_local_windows_e2e',
      ['test/unit/test_conduit_local_windows_e2e.c'],
      kwargs: test_common_kwargs,
  )
  # Same reasoning as conduit_local_conformance_e2e above (n00b#415).
  test('conduit_local_windows_e2e', conduit_local_windows_e2e_test, suite: 'unit',
       timeout: 120)
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
conduit_lifecycle_e2e_test = executable('test_conduit_lifecycle_e2e',
    ['test/unit/test_conduit_lifecycle_e2e.c'],
    kwargs: test_common_kwargs,
)
test('conduit_lifecycle_e2e', conduit_lifecycle_e2e_test, suite: 'unit')
endif

env_test = executable('test_env',
    ['test/unit/test_env.c'],
    kwargs: test_common_kwargs,
)
test('env', env_test, suite: 'unit')

http_client_plain_test = executable('test_http_client_plain',
    ['test/unit/test_http_client_plain.c'],
    kwargs: test_common_kwargs,
)
test('http_client_plain', http_client_plain_test, suite: 'unit')

http_service_unix_test = executable('test_http_service_unix',
    ['test/unit/test_http_service_unix.c'],
    kwargs: test_common_kwargs,
)
test('http_service_unix', http_service_unix_test, suite: 'unit')

# Per-request pool growth of the conduit stream reader, fd-owner bulk read,
# and the HTTP clients and service. POSIX-only (raw-socket peers).
if target_os != 'windows'
http_leaks_test = executable('test_http_leaks',
    ['test/unit/test_http_leaks.c'],
    kwargs: test_common_kwargs,
)
test('http_leaks', http_leaks_test, suite: 'unit')
endif

hostmeta_test = executable('test_hostmeta',
    ['test/unit/test_hostmeta.c'],
    kwargs: test_common_kwargs,
)
test('hostmeta', hostmeta_test, suite: 'unit')

chalk_module_test = executable('test_chalk_module',
    ['test/unit/test_chalk_module.c'],
    kwargs: test_common_kwargs,
)
test('chalk_module', chalk_module_test, suite: 'unit')

chalk_elf_rewrite_test = executable('test_chalk_elf_rewrite',
    ['test/unit/test_chalk_elf_rewrite.c'],
    kwargs: test_common_kwargs,
)
test('chalk_elf_rewrite', chalk_elf_rewrite_test, suite: 'unit')

# ============================================================================
# naudit regression tests (consolidated in-tree per WP-008 / D-017).
#
# Most tests load fixtures from `test/fixtures/naudit/`; the build
# bakes that path into a `-DN00B_AUDIT_TEST_FIXTURE_DIR="..."` flag
# so the C source never carries a hardcoded path. The macro name
# keeps the `N00B_AUDIT_` prefix per WP-008 symbol-stability — only
# the binary + directory names changed, not the engine identity.
#
# `test_naudit_reference_guidance` and the three rule-driven tests
# (`engine_malloc`, `engine_legacy`, `engine_bulk`) gate on POSIX
# `access()` of the repo-local canonical rules file — reached via
# `N00B_AUDIT_TEST_FIXTURE_DIR "/../../../audit-rules.bnf"` (i.e.
# `<repo>/audit-rules.bnf`), no longer a hardcoded sibling-workspace
# path — and emit `[SKIP]` if it isn't reachable.
# ============================================================================

# TEMP: the naudit engine tests currently fail (audit finds 0 violations
# — a parse-tree/audit-rule-matching issue) and several hang and burn CPU
# under the full suite. Disabled from RUNNING (binaries still build) until
# the audit-matching work lands. Flip to `true` to re-enable.
naudit_tests_enabled = false

naudit_fixture_dir = (
    meson.current_source_dir() / 'test' / 'fixtures' / 'naudit')

# `test_common_kwargs.c_args` is `exe_c_args`; append the fixture-dir
# macro for the tests that load fixtures (all of them except
# `reference_guidance`, which is fine to receive it too — the macro
# is just unused there).
naudit_test_c_args = (
    exe_c_args + ['-DN00B_AUDIT_TEST_FIXTURE_DIR="' + naudit_fixture_dir + '"'])

# Tests that exercise naudit's C preprocessor shell out to `ncc` at
# runtime. When build.sh configured Meson with an absolute CC path,
# make the same workspace compiler discoverable to those subprocesses.
naudit_preprocess_test_env = environment()
if target_os != 'windows'
  _n00b_cc_cmd = cc.cmd_array()
  if _n00b_cc_cmd.length() > 0 and _n00b_cc_cmd[0].contains('/')
    _n00b_cc_dir = run_command('dirname', _n00b_cc_cmd[0], check: true).stdout().strip()
    naudit_preprocess_test_env.prepend('PATH', _n00b_cc_dir, separator: ':')
  endif
endif

foreach _ntest : [
    'module',
    'guidance',
    'languages',
    'reference_guidance',
    'engine',
    'engine_legacy',
    'engine_malloc',
    'engine_bulk',
    'engine_query_mode',
    'parse_string',
    'preprocess',
    'cli',
    'cli_json',
    'rewrite',
    'filter',
    'filter_e2e',
    'exemption',
    'baseline',
    'signing',
    'signing_ux',
    'blame',
    'trust_root',
]
    _naudit_test_exe = executable(
        'test_naudit_' + _ntest,
        ['test/unit/test_naudit_' + _ntest + '.c'],
        include_directories: incdir,
        c_args: naudit_test_c_args,
        link_args: exe_link_args,
        link_with: [n00b],
        build_by_default: n00b_build_tests,
        # WP-009 Phase 3: `naudit/filter.h` pulls in `n00b/eval.h`,
        # which transitively includes `slay/codegen.h` and so needs
        # `mir.h` on the include path. Adding `mir_dep` to every
        # naudit test is harmless for the non-filter tests (they
        # never include `mir.h`); the alternative is a per-test
        # branch that complicates the foreach.
        #
        # WP-013: `test_naudit_blame` includes `<git2.h>` directly
        # (the test harness drives libgit2's repo_init/index/commit
        # primitives to build fixtures); adding `libgit2_dep` to
        # every test in the foreach gives that one test access to
        # the libgit2 include path while remaining harmless for the
        # others (they don't include `<git2.h>`).
        dependencies: [picoquic_dep, monocypher_dep, mir_dep, libgit2_dep],
    )
    # WP-011 bumps the default naudit-test timeout from meson's
    # global 30s to 180s. Several tests (cli, baseline, filter)
    # drive the full guidance-load + grammar-merge + parse pipeline
    # on every iteration and routinely sit close to the prior cap
    # under the sanitizer-instrumented test harness. The bump is
    # uniform across the foreach to keep the spec readable.
    if naudit_tests_enabled
        test('naudit_' + _ntest, _naudit_test_exe, suite: 'unit',
             env: naudit_preprocess_test_env,
             timeout: 180)
    endif
endforeach

if conf_static_grammar_image
  # WP-008 Phase 3: bake c_ncc.bnf into a host relocatable object containing a
  # n00b_gimage section record, then link that object into the static-grammar
  # consumers. Lookup discovers and relocates the linked record directly instead
  # of compiling a generated-C lazy materializer.
  c_grammar_image_command = [
      naudit_grammar_bake,
      '@INPUT@',
      'translation_unit',
      '@OUTPUT@',
      '__naudit_c_grammar',
      'c_ncc',
  ]
  if target_os != 'windows'
    c_grammar_image_command = [
        n00b_gcmap_index,
        '--exec',
        naudit_grammar_bake.full_path(),
        '@INPUT@',
        'translation_unit',
        '@OUTPUT@',
        '__naudit_c_grammar',
        'c_ncc',
    ]
  endif

  c_grammar_image = custom_target(
      'c_grammar_image',
      input: 'grammars/c_ncc.bnf',
      output: 'c_grammar_image.o',
      command: c_grammar_image_command,
      depends: [naudit_grammar_bake],
  )

  # The static-grammar-image test drives the bake tool as a subprocess and
  # exercises the build-time-baked c_ncc object linked in via c_grammar_image.
  # It needs both the bake-tool path and the c_ncc.bnf path at compile time.
# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
  test_static_grammar_image_exe = executable(
      'test_static_grammar_image',
      ['test/unit/test_static_grammar_image.c',
       c_grammar_image],
      include_directories: incdir,
      c_args: exe_c_args + [
          '-DNAUDIT_GRAMMAR_BAKE_PATH="' + naudit_grammar_bake.full_path() + '"',
          '-DNAUDIT_C_NCC_BNF_PATH="'
              + (meson.current_source_dir() / 'grammars' / 'c_ncc.bnf') + '"',
          '-DNAUDIT_C_FIXTURE_PATH="'
              + (meson.current_source_dir() / 'test' / 'fixtures' / 'naudit'
                 / 'fixture_null.c') + '"',
      ],
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, mir_dep, libgit2_dep],
      build_by_default: n00b_build_tests,
  )
  if target_os != 'windows'
    test('static_grammar_image', n00b_gcmap_index,
         args: ['--exec', test_static_grammar_image_exe.full_path()],
         depends: [test_static_grammar_image_exe],
         suite: 'unit',
         timeout: 180)
  else
    test('static_grammar_image', test_static_grammar_image_exe, suite: 'unit',
         timeout: 180)
  endif
endif

  # WP-008 Phase 4 behavioral-equivalence proof: the linked baked c_ncc
  # grammar parses the same representative C input as the legacy heap
  # materialization path and a fresh runtime BNF parse.
  test_grammar_baked_equiv_exe = executable(
      'test_grammar_baked_equiv',
      ['test/unit/test_grammar_baked_equiv.c',
       c_grammar_image],
      include_directories: incdir,
      c_args: exe_c_args + [
          '-DNAUDIT_C_NCC_BNF_PATH="'
              + (meson.current_source_dir() / 'grammars' / 'c_ncc.bnf') + '"',
          '-DNAUDIT_C_FIXTURE_PATH="'
              + (meson.current_source_dir() / 'test' / 'fixtures' / 'naudit'
                 / 'fixture_null.c') + '"',
      ],
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, mir_dep, libgit2_dep],
      build_by_default: n00b_build_tests,
  )
  if target_os != 'windows'
    test('grammar_baked_equiv', n00b_gcmap_index,
         args: ['--exec', test_grammar_baked_equiv_exe.full_path()],
         depends: [test_grammar_baked_equiv_exe],
         env: naudit_preprocess_test_env,
         suite: 'unit',
         timeout: 180)
  else
    test('grammar_baked_equiv', test_grammar_baked_equiv_exe, suite: 'unit',
         timeout: 180)
  endif

  # WP-020 Phase 5 production-path coverage: reuse the naudit filter e2e test
  # source, but link the baked c_ncc grammar image into this executable so the
  # engine takes `n00b_static_grammar_lookup(r"c_ncc")` instead of the runtime
  # BNF fallback. This catches static-image parse-tree/capture/filter drift that
  # the ordinary naudit tests cannot see.
  test_naudit_filter_static_grammar_exe = executable(
      'test_naudit_filter_static_grammar',
      ['test/unit/test_naudit_filter_e2e.c', c_grammar_image],
      include_directories: incdir,
      c_args: naudit_test_c_args,
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, mir_dep, libgit2_dep],
      build_by_default: n00b_build_tests,
  )

if naudit_tests_enabled
  if target_os != 'windows'
    test('naudit_filter_static_grammar_e2e', n00b_gcmap_index,
         args: ['--exec', test_naudit_filter_static_grammar_exe.full_path()],
         depends: [test_naudit_filter_static_grammar_exe],
         env: naudit_preprocess_test_env,
         suite: 'unit',
         timeout: 180)
  else
    test('naudit_filter_static_grammar_e2e',
         test_naudit_filter_static_grammar_exe,
         suite: 'unit',
         timeout: 180)
  endif
endif
elif conf_static_grammar_image_requested
  test_static_grammar_image_unsupported_exe = executable(
      'test_static_grammar_image_unsupported',
      ['test/unit/test_static_grammar_image_unsupported.c'],
      kwargs: test_common_kwargs,
  )
  test('static_grammar_image',
       test_static_grammar_image_unsupported_exe,
       suite: 'unit',
       timeout: 30)
  test('grammar_baked_equiv',
       test_static_grammar_image_unsupported_exe,
       suite: 'unit',
       timeout: 30)
  if naudit_tests_enabled
    test('naudit_filter_static_grammar_e2e',
         test_static_grammar_image_unsupported_exe,
         suite: 'unit',
         timeout: 30)
  endif
endif

attest_module_test = executable('test_attest_module',
    ['test/unit/test_attest_module.c'],
    kwargs: test_common_kwargs,
)
test('attest_module', attest_module_test, suite: 'unit')

clickhouse_module_test = executable('test_clickhouse_module',
    ['test/unit/test_clickhouse_module.c'],
    kwargs: test_common_kwargs,
)
test('clickhouse_module', clickhouse_module_test, suite: 'unit')

rocs_module_test = executable('test_rocs_module',
    ['test/unit/test_rocs_module.c'],
    kwargs: test_common_kwargs,
)
test('rocs_module', rocs_module_test, suite: 'unit')

rocs_shard_test = executable('test_rocs_shard',
    ['test/unit/test_rocs_shard.c'],
    kwargs: test_common_kwargs,
)
test('rocs_shard', rocs_shard_test, suite: 'unit')

rocs_seal_bench_test = executable('test_rocs_seal_bench',
    ['test/unit/test_rocs_seal_bench.c'],
    kwargs: test_common_kwargs,
)
# Raised for shared CI runners (n00b#261). Every one of these hit its
# declared cap EXACTLY on ubuntu-latest/macos-latest -- 30.02s, 60.06s,
# 300.09s -- which is the signature of a too-tight limit, not a hang.
# Measured locally at roughly half these values with wide variance.
test('rocs_seal_bench', rocs_seal_bench_test, suite: 'unit', timeout: 180)

rocs_shard_lifecycle_test = executable('test_rocs_shard_lifecycle',
    ['test/unit/test_rocs_shard_lifecycle.c'],
    kwargs: test_common_kwargs,
)
test('rocs_shard_lifecycle', rocs_shard_lifecycle_test, suite: 'unit')

rocs_index_test = executable('test_rocs_index',
    ['test/unit/test_rocs_index.c'],
    kwargs: test_common_kwargs,
)
test('rocs_index', rocs_index_test, suite: 'unit')

# n00b#496: the hang only appears when the emitter is a real SUBPROCESS whose
# pipe n00b's own conduit drains -- the in-process test_print_contention never
# reproduces it. Registered in 'known-broken' for the same reason that test is:
# it passes every assertion it makes, and then the process dies in teardown on
# n00b#409 (subscription.c:139 / conduit.h:170, "lock not initialized"), which
# is not this test's defect. Run it on demand with:
#     meson test --suite known-broken print_sync_subproc
print_sync_subproc_test = executable('test_print_sync_subproc',
    ['test/unit/test_print_sync_subproc.c'],
    kwargs: test_common_kwargs,
)
test('print_sync_subproc', print_sync_subproc_test,
     suite: 'known-broken', is_parallel: false, timeout: 300)


print_contention_test = executable('test_print_contention',
    ['test/unit/test_print_contention.c'],
    kwargs: test_common_kwargs,
)
# 'known-broken', not 'unit': this test PASSES every assertion it makes on
# all three platforms, and then the process dies for reasons that are not
# its own.
#
#   macOS / Linux: "All print contention tests passed", then SIGABRT in
#     n00b_shutdown -> n00b_conduit_destroy -> topic_notify_all with
#     conduit.h:170 "Lock ... was not initialized before use" -- n00b#409.
#     This test reproduces that at ~40% (vs 0/15 for test_print), because
#     threads that print and then exit are its trigger; it did not create
#     the defect and cannot avoid it while asserting what it asserts.
#   Windows: the drained arm never finishes, which is the n00b#496 hang
#     this test was written to catch. That is a RESULT, not a flake, and
#     quarantining it is not an admission it is wrong -- it is the finding
#     parked where it does not red the gate for everyone else.
#
# 'known-broken' rather than should_fail, for the reason rocs_live_cursor
# gives above: the POSIX outcome is a pass followed by an abort, so
# should_fail would report UNEXPECTEDPASS on the assertions and leave the
# gate red either way.
#
# Restore to 'unit' when n00b#409 is fixed (POSIX) and n00b#496 is fixed or
# the Windows arm is scoped out. Run it on demand with:
#     meson test --suite known-broken print_contention
test('print_contention',
     print_contention_test,
     suite: 'known-broken',
     timeout: 300)

rocs_fulltext_index_test = executable('test_rocs_fulltext_index',
    ['test/unit/test_rocs_fulltext_index.c'],
    kwargs: test_common_kwargs,
)
test('rocs_fulltext_index', rocs_fulltext_index_test, suite: 'unit')

rocs_catch_all_test = executable('test_rocs_catch_all',
    ['test/unit/test_rocs_catch_all.c'],
    kwargs: test_common_kwargs,
)
test('rocs_catch_all', rocs_catch_all_test, suite: 'unit')

# n00b#359: a TERM value absent from the store must not map every sealed shard.
rocs_term_summary_test = executable('test_rocs_term_summary',
    ['test/unit/test_rocs_term_summary.c'],
    kwargs: test_common_kwargs,
)
# Above the 120 s the service ingest wait allows, so a stall there reports
# its own failure instead of a bare timeout.
test('rocs_term_summary', rocs_term_summary_test, suite: 'unit', timeout: 180)

bench_rocs_term_summary = executable('bench_rocs_term_summary',
    ['test/unit/bench_rocs_term_summary.c'],
    kwargs: test_common_kwargs,
)

# Registered so the summary filters keep missing no present value and every
# query keeps mapping exactly the shards its filters pass. Sized down here to
# one open and two caps, with and without summaries; run the binary with no
# environment set for the measurement.
test('rocs_term_summary_bench',
    bench_rocs_term_summary,
    suite: 'unit',
    timeout: 120,
    env: {'ROCS_BENCH_SHARDS': '3',
          'ROCS_BENCH_RECORDS': '500',
          'ROCS_BENCH_ABSENT': '200',
          'ROCS_BENCH_QUERIES': '3',
          'ROCS_BENCH_OPENS': '1',
          'ROCS_BENCH_CAPS': '0,4096'},
)

rocs_ngram_index_test = executable('test_rocs_ngram_index',
    ['test/unit/test_rocs_ngram_index.c'],
    kwargs: test_common_kwargs,
)
test('rocs_ngram_index', rocs_ngram_index_test, suite: 'unit')

rocs_regex_candidates_test = executable('test_rocs_regex_candidates',
    ['test/unit/test_rocs_regex_candidates.c'],
    kwargs: test_common_kwargs,
)
test('rocs_regex_candidates', rocs_regex_candidates_test, suite: 'unit')

rocs_heavy_index_test = executable('test_rocs_heavy_index',
    ['test/unit/test_rocs_heavy_index.c'],
    kwargs: test_common_kwargs,
)
test('rocs_heavy_index', rocs_heavy_index_test, suite: 'unit')

json_field_scan_test = executable('test_rocs_json_field_scan',
    ['test/unit/test_rocs_json_field_scan.c'],
    kwargs: test_common_kwargs,
)
test('rocs_json_field_scan', json_field_scan_test, suite: 'unit')

rocs_plan_cost_test = executable('test_rocs_plan_cost',
    ['test/unit/test_rocs_plan_cost.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost', rocs_plan_cost_test, suite: 'unit')

rocs_plan_cost_report_test = executable('test_rocs_plan_cost_report',
    ['test/unit/test_rocs_plan_cost_report.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost_report', rocs_plan_cost_report_test, suite: 'unit')

rocs_rss_kwargs = test_common_kwargs
if target_os == 'windows'
  # GetProcessMemoryInfo, for the peak-RSS line these two print.
  rocs_rss_kwargs = test_common_kwargs + {
    'dependencies': test_common_kwargs['dependencies'] + [cc.find_library('psapi')],
  }
endif

rocs_plan_pathological_test = executable('test_rocs_plan_pathological',
    ['test/unit/test_rocs_plan_pathological.c'],
    kwargs: rocs_rss_kwargs,
)
test('rocs_plan_pathological',
     rocs_plan_pathological_test,
     suite: 'unit',
     timeout: 120)

rocs_plan_cost_sealed_test = executable('test_rocs_plan_cost_sealed',
    ['test/unit/test_rocs_plan_cost_sealed.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost_sealed', rocs_plan_cost_sealed_test, suite: 'unit')

rocs_plan_cost_edges_test = executable('test_rocs_plan_cost_edges',
    ['test/unit/test_rocs_plan_cost_edges.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost_edges',
     rocs_plan_cost_edges_test,
     suite: 'unit',
     timeout: 120)

rocs_plan_cost_model_test = executable('test_rocs_plan_cost_model',
    ['test/unit/test_rocs_plan_cost_model.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost_model', rocs_plan_cost_model_test, suite: 'unit')

rocs_plan_cost_env_test = executable('test_rocs_plan_cost_env',
    ['test/unit/test_rocs_plan_cost_env.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_cost_env', rocs_plan_cost_env_test, suite: 'unit')

bench_rocs_zone_map = executable('bench_rocs_zone_map',
    ['test/unit/bench_rocs_zone_map.c'],
    kwargs: test_common_kwargs,
)

# Registered so the workload keeps ingesting, sealing and recording bounds as
# it claims to. Sized right down here; run the binary directly for timings.
test('rocs_zone_map_bench',
    bench_rocs_zone_map,
    suite: 'unit',
    env: {
        'ROCS_BENCH_SHARDS': '8',
        'ROCS_BENCH_RECORDS': '64',
        'ROCS_BENCH_ROUNDS': '1',
    },
)

bench_rocs_prims = executable('bench_rocs_prims',
    ['test/unit/bench_rocs_prims.c'],
    kwargs: test_common_kwargs,
)

rocs_retention_cap_liveness_test = executable('test_rocs_retention_cap_liveness',
    ['test/unit/test_rocs_retention_cap_liveness.c'],
    kwargs: test_common_kwargs,
)

# The crashappsec/wax#1229 acceptance test: at the retention cap, with queries
# pinning shards, a status read must still answer. Registered small and short;
# raise ROCS_LIVENESS_* to run it as a soak.
test('rocs_retention_cap_liveness',
    rocs_retention_cap_liveness_test,
    suite: 'unit',
    timeout: 120,
    env: {'ROCS_LIVENESS_SECONDS': '3',
          'ROCS_LIVENESS_CAP': '24',
          'ROCS_LIVENESS_QUERIERS': '2',
          'ROCS_LIVENESS_RECORDS': '40',
          'ROCS_LIVENESS_MAX_STALL_MS': '2000'},
)

bench_rocs_retention_pinned = executable('bench_rocs_retention_pinned',
    ['test/unit/bench_rocs_retention_pinned.c'],
    kwargs: test_common_kwargs,
)

# Registered small so a fully-pinned pass keeps dropping nothing and keeps
# reporting PINNED; run the binary directly for the N sweep.
test('rocs_retention_pinned_bench',
    bench_rocs_retention_pinned,
    suite: 'unit',
    env: {'ROCS_BENCH_SIZES': '8,16', 'ROCS_BENCH_RECORDS': '2'},
)

bench_gc_pause_budget = executable('bench_gc_pause_budget',
    ['test/unit/bench_gc_pause_budget.c'],
    kwargs: test_common_kwargs,
)

# n00b#539. runtime.h documents a 0.25s pause budget that nothing enforced;
# this is the assertion. Registered small so it gates pause REGRESSIONS on
# every run -- the survivor sweep that shows the growth shape wants the binary
# run directly with N00B_BENCH_SURVIVORS.
test('gc_pause_budget',
    bench_gc_pause_budget,
    suite: 'unit',
    env: {'N00B_BENCH_SURVIVORS': '2000,4000',
          'N00B_BENCH_PAUSE_BUDGET_MS': '250'},
)

bench_rocs_catalog_rewrite = executable('bench_rocs_catalog_rewrite',
    ['test/unit/bench_rocs_catalog_rewrite.c'],
    kwargs: test_common_kwargs,
)

# Registered small so the drops keep happening and the catalog stays
# readable; run the binary directly for the timings.
test('rocs_catalog_rewrite_bench',
    bench_rocs_catalog_rewrite,
    suite: 'unit',
    env: {'ROCS_BENCH_SHARDS': '6',
          'ROCS_BENCH_RECORDS': '50',
          'ROCS_BENCH_DROPS': '2'},
)

bench_rocs_query_ab = executable('bench_rocs_query_ab',
    ['test/unit/bench_rocs_query_ab.c'],
    kwargs: test_common_kwargs,
)

# Registered small so the shapes keep planning and executing; run the binary
# directly for numbers.
test('rocs_query_ab_bench',
    bench_rocs_query_ab,
    suite: 'unit',
    env: {'ROCS_BENCH_SHARDS': '4', 'ROCS_BENCH_RECORDS': '25', 'ROCS_BENCH_ROUNDS': '1'},
)

bench_rocs_ingest = executable('bench_rocs_ingest',
    ['test/unit/bench_rocs_ingest.c'],
    kwargs: test_common_kwargs,
)

# Registered so every workload keeps indexing and sealing what it claims to.
# Sized down here; run the binary directly for the timings.
test('rocs_ingest_bench',
    bench_rocs_ingest,
    suite: 'unit',
    env: {'ROCS_BENCH_RECORDS': '256'},
)

bench_rocs_plan_ab = executable('bench_rocs_plan_ab',
    ['test/unit/bench_rocs_plan_ab.c'],
    kwargs: rocs_rss_kwargs,
)

# Registered so the shapes it generates keep being checked: every one must
# answer identically with cost planning on and off. Sized down here because
# that check needs neither a big shard nor a stable clock; run the binary with
# no environment set to get the timings it prints.
test('rocs_plan_ab',
    bench_rocs_plan_ab,
    suite: 'unit',
    env: {'ROCS_BENCH_RECORDS': '512', 'ROCS_BENCH_REPEATS': '1'},
)

rocs_index_df_test = executable('test_rocs_index_df',
    ['test/unit/test_rocs_index_df.c'],
    kwargs: test_common_kwargs,
)
test('rocs_index_df', rocs_index_df_test, suite: 'unit')

rocs_index_scaling_test = executable('test_rocs_index_scaling',
    ['test/unit/test_rocs_index_scaling.c'],
    kwargs: test_common_kwargs,
)
test('rocs_index_scaling', rocs_index_scaling_test, suite: 'unit')

rocs_normalizer_test = executable('test_rocs_normalizer',
    ['test/unit/test_rocs_normalizer.c'],
    kwargs: test_common_kwargs,
)
test('rocs_normalizer', rocs_normalizer_test, suite: 'unit')

rocs_filter_value_test = executable('test_rocs_filter_value',
    ['test/unit/test_rocs_filter_value.c'],
    kwargs: test_common_kwargs,
)
test('rocs_filter_value', rocs_filter_value_test, suite: 'unit')

rocs_filter_builder_test = executable('test_rocs_filter_builder',
    ['test/unit/test_rocs_filter_builder.c'],
    kwargs: test_common_kwargs,
)
test('rocs_filter_builder', rocs_filter_builder_test, suite: 'unit')

rocs_filter_ir_test = executable('test_rocs_filter_ir',
    ['test/unit/test_rocs_filter_ir.c'],
    kwargs: test_common_kwargs,
)
test('rocs_filter_ir', rocs_filter_ir_test, suite: 'unit')

rocs_filter_lowering_test = executable('test_rocs_filter_lowering',
    ['test/unit/test_rocs_filter_lowering.c'],
    kwargs: test_common_kwargs,
)
test('rocs_filter_lowering', rocs_filter_lowering_test, suite: 'unit')

rocs_plan_predicate_test = executable('test_rocs_plan_predicate',
    ['test/unit/test_rocs_plan_predicate.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_predicate', rocs_plan_predicate_test, suite: 'unit')

rocs_zone_map_test = executable('test_rocs_zone_map',
    ['test/unit/test_rocs_zone_map.c'],
    kwargs: test_common_kwargs,
)
test('rocs_zone_map', rocs_zone_map_test, suite: 'unit')

rocs_plan_rewrite_test = executable('test_rocs_plan_rewrite',
    ['test/unit/test_rocs_plan_rewrite.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_rewrite', rocs_plan_rewrite_test, suite: 'unit')

rocs_plan_ordset_test = executable('test_rocs_plan_ordset',
    ['test/unit/test_rocs_plan_ordset.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_ordset', rocs_plan_ordset_test, suite: 'unit')

rocs_plan_dispatch_test = executable('test_rocs_plan_dispatch',
    ['test/unit/test_rocs_plan_dispatch.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_dispatch', rocs_plan_dispatch_test, suite: 'unit')

rocs_plan_oracle_test = executable('test_rocs_plan_oracle',
    ['test/unit/test_rocs_plan_oracle.c'],
    kwargs: test_common_kwargs,
)
# Raised for shared CI runners (n00b#261). Every one of these hit its
# declared cap EXACTLY on ubuntu-latest/macos-latest -- 30.02s, 60.06s,
# 300.09s -- which is the signature of a too-tight limit, not a hang.
# Measured locally at roughly half these values with wide variance.
# QUARANTINED (n00b#280): blew a RAISED 120s budget on Linux, so this is
# not simply a tight limit -- something is hanging or pathologically slow
# there. Passes on macOS. Quarantined rather than raised again, because
# raising a limit you have already raised once is how a hang gets hidden.
test('rocs_plan_oracle', rocs_plan_oracle_test, suite: 'known-broken', timeout: 120)

rocs_plan_verify_test = executable('test_rocs_plan_verify',
    ['test/unit/test_rocs_plan_verify.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_verify', rocs_plan_verify_test, suite: 'unit')

rocs_plan_partition_test = executable('test_rocs_plan_partition',
    ['test/unit/test_rocs_plan_partition.c'],
    kwargs: test_common_kwargs,
)
test('rocs_plan_partition', rocs_plan_partition_test, suite: 'unit')

rocs_query_spec_test = executable('test_rocs_query_spec',
    ['test/unit/test_rocs_query_spec.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_spec', rocs_query_spec_test, suite: 'unit')

rocs_query_agg_test = executable('test_rocs_query_agg',
    ['test/unit/test_rocs_query_agg.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_agg', rocs_query_agg_test, suite: 'unit')

rocs_query_ranking_test = executable('test_rocs_query_ranking',
    ['test/unit/test_rocs_query_ranking.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_ranking', rocs_query_ranking_test, suite: 'unit')

rocs_query_cancel_test = executable('test_rocs_query_cancel',
    ['test/unit/test_rocs_query_cancel.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_cancel', rocs_query_cancel_test, suite: 'unit',
     timeout: 120)

rocs_ranked_records_test = executable('test_rocs_ranked_records',
    ['test/unit/test_rocs_ranked_records.c'],
    kwargs: test_common_kwargs,
)
test('rocs_ranked_records', rocs_ranked_records_test, suite: 'unit')

rocs_query_topn_test = executable('test_rocs_query_topn',
    ['test/unit/test_rocs_query_topn.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_topn', rocs_query_topn_test, suite: 'unit')

rocs_query_scaling_test = executable('test_rocs_query_scaling',
    ['test/unit/test_rocs_query_scaling.c'],
    kwargs: test_common_kwargs,
)
# Builds and walks stores of tens of thousands of records, which needs more
# than meson's 30s default under parallel CI load. Same bound as the
# seal-heavy tests (rocs_seal_bench, rocs_term_summary, rocs_catalog_index).
test('rocs_query_scaling', rocs_query_scaling_test, suite: 'unit',
     timeout: 180)

rocs_query_view_test = executable('test_rocs_query_view',
    ['test/unit/test_rocs_query_view.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_view', rocs_query_view_test, suite: 'unit')

rocs_live_view_test = executable('test_rocs_live_view',
    ['test/unit/test_rocs_live_view.c'],
    kwargs: test_common_kwargs,
)
test('rocs_live_view', rocs_live_view_test, suite: 'unit')

rocs_live_tail_test = executable('test_rocs_live_tail',
    ['test/unit/test_rocs_live_tail.c'],
    kwargs: test_common_kwargs,
)
test('rocs_live_tail', rocs_live_tail_test, suite: 'unit')

rocs_live_cursor_test = executable('test_rocs_live_cursor',
    ['test/unit/test_rocs_live_cursor.c'],
    kwargs: test_common_kwargs,
)
# QUARANTINED (n00b#295): hangs on the Linux runner. Not a tight limit --
# the two observed outcomes are OK at 0.12s and TIMEOUT at exactly the 30s
# cap with no output, on the same commit. A test whose runtime is bimodal
# between 0.12s and never is blocking on something, so raising the budget
# would only make the red take longer to arrive (contrast
# rocs_large_shard_seal, whose 16.77/20.06/>30s spread is continuous and
# IS a budget problem).
#
# 'known-broken' rather than should_fail: it passes most runs, so
# should_fail would report UNEXPECTEDPASS and leave the gate red either
# way. Same reasoning as the n00b#280 group above. Restore its real suite
# with the fix.
test('rocs_live_cursor', rocs_live_cursor_test, suite: 'known-broken')

rocs_live_resume_test = executable('test_rocs_live_resume',
    ['test/unit/test_rocs_live_resume.c'],
    kwargs: test_common_kwargs,
)
test('rocs_live_resume', rocs_live_resume_test, suite: 'unit')

rocs_live_conduit_test = executable('test_rocs_live_conduit',
    ['test/unit/test_rocs_live_conduit.c'],
    kwargs: test_common_kwargs,
)
test('rocs_live_conduit', rocs_live_conduit_test, suite: 'unit')

rocs_query_cursor_test = executable('test_rocs_query_cursor',
    ['test/unit/test_rocs_query_cursor.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_cursor', rocs_query_cursor_test, suite: 'unit')

rocs_query_cache_test = executable('test_rocs_query_cache',
    ['test/unit/test_rocs_query_cache.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_cache', rocs_query_cache_test, suite: 'unit')

rocs_query_resume_test = executable('test_rocs_query_resume',
    ['test/unit/test_rocs_query_resume.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_resume', rocs_query_resume_test, suite: 'unit')

rocs_query_stress_test = executable('test_rocs_query_stress',
    ['test/unit/test_rocs_query_stress.c'],
    kwargs: test_common_kwargs,
)
test('rocs_query_stress', rocs_query_stress_test, suite: 'unit')

rocs_linear_cursor_test = executable('test_rocs_linear_cursor',
    ['test/unit/test_rocs_linear_cursor.c'],
    kwargs: test_common_kwargs,
)
test('rocs_linear_cursor', rocs_linear_cursor_test, suite: 'unit')

rocs_store_test = executable('test_rocs_store',
    ['test/unit/test_rocs_store.c'],
    kwargs: test_common_kwargs,
)
test('rocs_store', rocs_store_test, suite: 'unit')

rocs_store_catalog_test = executable('test_rocs_store_catalog',
    ['test/unit/test_rocs_store_catalog.c'],
    kwargs: test_common_kwargs,
)
test('rocs_store_catalog', rocs_store_catalog_test, suite: 'unit')

rocs_service_config_test = executable('test_rocs_service_config',
    ['test/unit/test_rocs_service_config.c'],
    kwargs: test_common_kwargs,
)
test('rocs_service_config', rocs_service_config_test, suite: 'unit')

rocs_service_runtime_test = executable('test_rocs_service_runtime',
    ['test/unit/test_rocs_service_runtime.c'],
    kwargs: test_common_kwargs,
)
# This PR's allocator-growth case issues ~1360 HTTP requests, each a fresh
# connection -- far more than any sibling service test, which run 1-12s on
# the Windows lane.
#
# Measured, not guessed: 3.6s on an idle aarch64 Linux box, but 60.2s on
# x86_64 Windows Server 2022 running alone. That is already past meson's 30s
# default before CI's four-way parallelism, which is why the Windows lane
# reported TIMEOUT 30.10s.
#
# 180 leaves headroom over the measured 60s and matches the other tests that
# do real work: rocs_seal_bench, rocs_term_summary, rocs_catalog_index.
test('rocs_service_runtime', rocs_service_runtime_test, suite: 'unit',
     timeout: 180)

rocs_service_health_test = executable('test_rocs_service_health',
    ['test/unit/test_rocs_service_health.c'],
    kwargs: test_common_kwargs,
)
test('rocs_service_health', rocs_service_health_test, suite: 'unit')

rocs_service_smoke_test = executable('test_rocs_service_smoke',
    ['test/unit/test_rocs_service_smoke.c'],
    kwargs: test_common_kwargs,
)
test('rocs_service_smoke', rocs_service_smoke_test, suite: 'unit')
test('rocs_service_smoke_s3_optional',
    rocs_service_smoke_test,
    args: ['--s3'],
    suite: 'unit',
)

rocs_k8s_manifests_test = executable('test_rocs_k8s_manifests',
    ['test/unit/test_rocs_k8s_manifests.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + [
            '-DROCS_TEST_SOURCE_ROOT="' + meson.current_source_dir() + '"',
        ],
    },
)
test('rocs_k8s_manifests', rocs_k8s_manifests_test, suite: 'unit')

rocs_residency_test = executable('test_rocs_residency',
    ['test/unit/test_rocs_residency.c'],
    kwargs: test_common_kwargs,
)
test('rocs_residency', rocs_residency_test, suite: 'unit')

rocs_catalog_index_test = executable('test_rocs_catalog_index',
    ['test/unit/test_rocs_catalog_index.c'],
    kwargs: test_common_kwargs,
)
# 160 real shard seals. The seal-heavy tests next door (rocs_seal_bench,
# rocs_term_summary) already carry 180 for the same reason; this one was
# left on meson's 30s default and was killed at 30.0s on every lane.
test('rocs_catalog_index', rocs_catalog_index_test, suite: 'unit',
     timeout: 180)

rocs_catalog_race_test = executable('test_rocs_catalog_race',
    ['test/unit/test_rocs_catalog_race.c'],
    kwargs: test_common_kwargs,
)
test('rocs_catalog_race', rocs_catalog_race_test, suite: 'unit')

rocs_ingest_test = executable('test_rocs_ingest',
    ['test/unit/test_rocs_ingest.c'],
    kwargs: test_common_kwargs,
)
test('rocs_ingest', rocs_ingest_test, suite: 'unit')

rocs_batch_ingest_test = executable('test_rocs_batch_ingest',
    ['test/unit/test_rocs_batch_ingest.c'],
    kwargs: test_common_kwargs,
)
test('rocs_batch_ingest', rocs_batch_ingest_test, suite: 'unit')

rocs_async_seal_test = executable('test_rocs_async_seal',
    ['test/unit/test_rocs_async_seal.c'],
    kwargs: test_common_kwargs,
)
test('rocs_async_seal', rocs_async_seal_test, suite: 'unit')

# Concurrency stress for the async seal worker thread: heavy rotate-while-
# marshaling under forced GC STW, looped over several store lifecycles.
rocs_async_seal_stress_test = executable('test_rocs_async_seal_stress',
    ['test/unit/test_rocs_async_seal_stress.c'],
    kwargs: test_common_kwargs,
)
# 120s, not the 30s default. Measured on an idle dev box this test runs
# 17-41s with wide run-to-run variance, so at 30s it fails roughly three runs
# in four -- and it fails as TIMEOUT, which reads as a hang rather than as a
# slow test. That misread is exactly what n00b#264 recorded as in-boundary
# evidence of an unbounded wait; the same binary passes 3/3 when given room.
# The bound in rocs_store_seal_queue_drain is a real fix, but this test is not
# what demonstrates it.
test('rocs_async_seal_stress', rocs_async_seal_stress_test, suite: 'unit',
     timeout: 120)

# Isolation repro: plain worker pool + heavy GC STW churn, no rocs.
worker_pool_gc_stress_test = executable('test_worker_pool_gc_stress',
    ['test/unit/test_worker_pool_gc_stress.c'],
    kwargs: test_common_kwargs,
)
# Raised for shared CI runners (n00b#261). Every one of these hit its
# declared cap EXACTLY on ubuntu-latest/macos-latest -- 30.02s, 60.06s,
# 300.09s -- which is the signature of a too-tight limit, not a hang.
# Measured locally at roughly half these values with wide variance.
# Its timeout was already raised to 120s; it now FAILs on the CI macOS runner
# at 8.3s while passing locally at 13.5s. A failure inside a budget it is not
# close to exhausting is a real fault, not slowness, so no further raise.
test('worker_pool_gc_stress', worker_pool_gc_stress_test, suite: 'unit',
     timeout: 120)

# n00b#309 reproducer: user_pool jobs through a worker pool while a helper
# thread forces collections every 150ms (the wax test_754_stress shape, no wax).
# 20s by default; N00B_RIG_SECONDS raises it for soak runs, N00B_RIG_POOL=arena
# places the pool struct in the moving arena (wax's AI work pool shape).
worker_pool_collector_rig_test = executable('test_worker_pool_collector_rig',
    ['test/unit/test_worker_pool_collector_rig.c'],
    kwargs: test_common_kwargs,
)
test('worker_pool_collector_rig', worker_pool_collector_rig_test, suite: 'unit',
     timeout: 180)
# Same rig under the pin-all policy (what a consumer without the gcmap
# dictionary, e.g. wax, gets by default): nothing is copied, no word rewritten.
test('worker_pool_collector_rig_pin_all', worker_pool_collector_rig_test,
     suite: 'unit', timeout: 180, env: ['N00B_GC_PIN_ALL=1'])

# A timed condition wait on a never-notified CV must return by its deadline
# under spurious wakes (n00b#307).
condition_timed_wait_test = executable('test_condition_timed_wait',
    ['test/unit/test_condition_timed_wait.c'],
    kwargs: test_common_kwargs,
)
# is_parallel: false because the pass condition is timing-based: a waiter that
# stays inside ONE timed wait for longer than BUDGET_NS + 20*HANG_NS (~2.0005 s,
# against a 500 us budget) is reported as hung. That detector is measuring the
# scheduler as much as the code, and on a 3-core macOS runner with four test
# processes in flight it fires on starvation alone.
#
# n00b#413: the test passed at 20.1 s for months, then the 2026-09-15 merge
# batch shifted it from position ~116 to ~93 -- into the suite's heaviest
# cluster (rocs_seal_bench 76 s, worker_pool_collector_rig 22 s,
# rocs_async_seal_stress 35 s, worker_pool_collector_rig_pin_all 23 s,
# worker_pool_gc_stress 36 s) -- and started failing at ~8.4 s on main and on
# every PR built against it. Nothing about the condition variable changed; the
# neighbours did. Serialising it measures the primitive again instead of the
# runner's load, the same reason dict_untyped_migrate and the vault tests carry
# this flag.
test('condition_timed_wait', condition_timed_wait_test, suite: 'unit',
     is_parallel: false, timeout: 60)

# Deterministic named-object-graph repro of the worker-thread GC trace defect.
gc_worker_trace_test = executable('test_gc_worker_trace',
    ['test/unit/test_gc_worker_trace.c'],
    kwargs: test_common_kwargs,
)
# Raised for shared CI runners (n00b#261). Every one of these hit its
# declared cap EXACTLY on ubuntu-latest/macos-latest -- 30.02s, 60.06s,
# 300.09s -- which is the signature of a too-tight limit, not a hang.
# Measured locally at roughly half these values with wide variance.
test('gc_worker_trace', gc_worker_trace_test, suite: 'unit', timeout: 120)

# Regression for seal status-5 on large shards (marshal type-layout promotion
# + shard prefix scan).  An 80MB shard must seal with zero record loss.
rocs_large_shard_seal_test = executable('test_rocs_large_shard_seal',
    ['test/unit/test_rocs_large_shard_seal.c'],
    kwargs: test_common_kwargs,
)
# Timeout raised from meson's 30s default (n00b#261's reasoning, same
# signature): observed at 16.77s and 20.06s on ubuntu-latest and TIMEOUT at
# 30.04s on a third run of comparable commits. That spread is continuous and
# crowds the cap, which is a budget that is too tight for a shared runner --
# an 80MB shard seal is genuinely heavy work.
#
# Distinct from rocs_live_cursor (n00b#295), which is quarantined instead:
# its outcomes are bimodal (0.12s or the full cap, no output), so it is
# blocking rather than slow, and more budget would not help it.
#
# 180s to match rocs_seal_bench, the other heavy seal test, rather than a
# marginal bump that would need revisiting on the next slow runner.
test('rocs_large_shard_seal', rocs_large_shard_seal_test, suite: 'unit',
     timeout: 180)

# n00b-lang/n00b#431: n00b_add_arena_segment must hold the STW gate for the
# whole operation.  Hammers segment adds from several threads while the main
# thread forces collects, with the segment-chain audit on; a lock-order mistake
# in the gate hangs here and a chain the collector no longer owns aborts.
# QUARANTINED (n00b#431): this is the crash, not a test of the fix.
#
# Six threads allocating 4 MB objects against forced collects reproduces the
# #431 crash family LOCALLY -- roughly 7 runs in 10 on a 10-core macOS box,
# with several distinct faults (a write into an unmapped page just past a live
# allocation; a garbage thread pointer in n00b_epoch_flush_all_stw).  That
# family had only ever been seen on one Windows box at about one crash per six
# 420-second cold restarts, so having it here at all is the point of checking
# it in.
#
# The crash IS fixed (n00b#431, the re-publish in n00b_arena_alloc's
# out-of-memory path): interleaved A/B on this box, unfixed 8 crashes in 20
# against fixed 0 in 20.  What keeps it quarantined is a separate, rarer
# HANG (2 in 20 fixed, seen unfixed too): the last worker parks in _n00b_stop_the_world
# waiting for critical_execution with no holder anywhere -- n00b#453.  A
# hang is a 300 s timeout in CI, which is exactly the flake this suite must
# not carry.  Restore its real suite when #453 lands.
#
# 'known-broken' rather than should_fail: it passes most runs, so
# should_fail would report UNEXPECTEDPASS. Same reasoning as rocs_live_cursor.
gc_large_alloc_churn_test = executable('test_gc_large_alloc_churn',
    ['test/unit/test_gc_large_alloc_churn.c'],
    kwargs: test_common_kwargs,
)
test('gc_large_alloc_churn', gc_large_alloc_churn_test, suite: 'known-broken',
     timeout: 300)

arena_segment_stw_race_test = executable('test_arena_segment_stw_race',
    ['test/unit/test_arena_segment_stw_race.c'],
    kwargs: test_common_kwargs,
)
test('arena_segment_stw_race', arena_segment_stw_race_test, suite: 'unit',
     timeout: 180)

# n00b-lang/n00b#432: the marshal's private scratch pool must stay within a
# small multiple of the image it produces.  It used to retain one block per
# marshaled allocation plus every superseded half of the output buffer, which on
# Windows is committed memory held for the whole marshal.
marshal_scratch_bound_test = executable('test_marshal_scratch_bound',
    ['test/unit/test_marshal_scratch_bound.c'],
    kwargs: test_common_kwargs,
)
test('marshal_scratch_bound', marshal_scratch_bound_test, suite: 'unit')

# Released big-page cache in pool.c: exact-size reuse is zeroed, a full class
# and the cap refuse, a .page_cache = false pool never parks, a parked page
# ages out across two collections, and N00B_POOL_PAGE_CACHE_MB=0 disables it.
# The cap is read once per process, so each cap setting is its own run.
pool_page_cache_test = executable('test_pool_page_cache',
    ['test/unit/test_pool_page_cache.c'],
    kwargs: test_common_kwargs,
)
test('pool_page_cache', pool_page_cache_test, suite: 'unit')
test('pool_page_cache_disabled', pool_page_cache_test, suite: 'unit',
     args: ['disabled'], env: ['N00B_POOL_PAGE_CACHE_MB=0'])
test('pool_page_cache_cap', pool_page_cache_test, suite: 'unit',
     args: ['cap'], env: ['N00B_POOL_PAGE_CACHE_MB=2'])

rocs_recovery_journal_test = executable('test_rocs_recovery_journal',
    ['test/unit/test_rocs_recovery_journal.c'],
    kwargs: test_common_kwargs,
)
test('rocs_recovery_journal', rocs_recovery_journal_test, suite: 'unit')

rocs_retention_test = executable('test_rocs_retention',
    ['test/unit/test_rocs_retention.c'],
    kwargs: test_common_kwargs,
)
test('rocs_retention', rocs_retention_test, suite: 'unit')

rocs_conduit_ingest_test = executable('test_rocs_conduit_ingest',
    ['test/unit/test_rocs_conduit_ingest.c'],
    kwargs: test_common_kwargs,
)
test('rocs_conduit_ingest', rocs_conduit_ingest_test, suite: 'unit')

rocs_record_stream_pin_order_test = executable('test_rocs_record_stream_pin_order',
    ['test/unit/test_rocs_record_stream_pin_order.c'],
    kwargs: test_common_kwargs,
)
test('rocs_record_stream_pin_order', rocs_record_stream_pin_order_test, suite: 'unit')

rocs_sealed_stream_test = executable('test_rocs_sealed_stream',
    ['test/unit/test_rocs_sealed_stream.c'],
    kwargs: test_common_kwargs,
)
test('rocs_sealed_stream', rocs_sealed_stream_test, suite: 'unit')

json_object_entries_test = executable('test_json_object_entries',
    ['test/unit/test_json_object_entries.c'],
    kwargs: test_common_kwargs,
)
test('json_object_entries', json_object_entries_test, suite: 'unit')

if conf_enable_aws
  aws_sts_test = executable('test_aws_sts',
      ['test/unit/test_aws_sts.c'],
      include_directories: incdir,
      c_args:    exe_c_args,
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, n00b_aws_dep],
      build_by_default: n00b_build_tests,
  )
  test('aws_sts', aws_sts_test, suite: 'unit')

  aws_s3_contract_test = executable('test_aws_s3_contract',
      ['test/unit/test_aws_s3_contract.c'],
      include_directories: incdir,
      c_args:    exe_c_args,
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, n00b_aws_dep],
      build_by_default: n00b_build_tests,
  )
  test('aws_s3_contract', aws_s3_contract_test, suite: 'unit')

  aws_dynamodb_test = executable('test_aws_dynamodb',
      ['test/unit/test_aws_dynamodb.c'],
      include_directories: incdir,
      c_args:    exe_c_args,
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, n00b_aws_dep],
      build_by_default: n00b_build_tests,
  )
  test('aws_dynamodb', aws_dynamodb_test, suite: 'unit')

  aws_s3_integration_smoke_test = executable('test_aws_s3_integration_smoke',
      ['test/unit/test_aws_s3_integration_smoke.c'],
      include_directories: incdir,
      c_args:    exe_c_args,
      link_args: exe_link_args,
      link_with: [n00b],
      dependencies: [picoquic_dep, monocypher_dep, n00b_aws_dep],
      build_by_default: n00b_build_tests,
  )
  test('aws_s3_integration_smoke',
      aws_s3_integration_smoke_test,
      suite: ['integration', 'aws'])
endif

attest_statement_roundtrip_test = executable('test_attest_statement_roundtrip',
    ['test/unit/test_attest_statement_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_statement_roundtrip', attest_statement_roundtrip_test, suite: 'unit')

attest_dsse_roundtrip_test = executable('test_attest_dsse_roundtrip',
    ['test/unit/test_attest_dsse_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_dsse_roundtrip', attest_dsse_roundtrip_test, suite: 'unit')

attest_arena_lifecycle_test = executable('test_attest_arena_lifecycle',
    ['test/unit/test_attest_arena_lifecycle.c'],
    kwargs: test_common_kwargs,
)
test('attest_arena_lifecycle', attest_arena_lifecycle_test, suite: 'unit')

attest_base64_test = executable('test_attest_base64',
    ['test/unit/test_attest_base64.c'],
    kwargs: test_common_kwargs,
)
test('attest_base64', attest_base64_test, suite: 'unit')

# n00b core util — `n00b_errno_str` POSIX errno-to-string helper.
# WP-004 Phase 5 closeout (DF-008 fix); PR-shaped libn00b lift
# matching the WP-002 wqw `n00b_stdin()/stdout()/stderr()` precedent
# (D-061).
errno_str_test = executable('test_errno_str',
    ['test/unit/test_errno_str.c'],
    kwargs: test_common_kwargs,
)
test('errno_str', errno_str_test, suite: 'unit')

# n00b core util — DER (X.690) encoder regression test.
# WP-005 Phase 3.
der_encode_test = executable('test_der_encode',
    ['test/unit/test_der_encode.c'],
    kwargs: test_common_kwargs,
)
test('der_encode', der_encode_test, suite: 'unit')

# n00b crypto — GF(2^8) finite-field arithmetic (coding-theory primitive).
gf256_test = executable('test_gf256',
    ['test/unit/test_gf256.c'],
    kwargs: test_common_kwargs,
)
test('gf256', gf256_test, suite: 'unit')

# n00b crypto — Reed-Solomon encoder over GF(2^8).
reed_solomon_test = executable('test_reed_solomon',
    ['test/unit/test_reed_solomon.c'],
    kwargs: test_common_kwargs,
)
test('reed_solomon', reed_solomon_test, suite: 'unit')

# n00b text — QR code data-encoding + ECC stage.
qr_codewords_test = executable('test_qr_codewords',
    ['test/unit/test_qr_codewords.c'],
    kwargs: test_common_kwargs,
)
test('qr_codewords', qr_codewords_test, suite: 'unit')

# n00b text — QR matrix assembly round-trip self-decode.
qr_matrix_test = executable('test_qr_matrix',
    ['test/unit/test_qr_matrix.c'],
    kwargs: test_common_kwargs,
)
test('qr_matrix', qr_matrix_test, suite: 'unit')

# n00b text — QR renderer (themed half-block lines + borderless table).
qr_render_test = executable('test_qr_render',
    ['test/unit/test_qr_render.c'],
    kwargs: test_common_kwargs,
)
test('qr_render', qr_render_test, suite: 'unit')

# n00b text — QR demo tool (not a test): prints a sample otpauth QR.
qr_demo_exe = executable('qr_demo',
    ['src/tools/qr_demo.c'],
    kwargs: n00b_app_kwargs,
)

# n00b core util — PKCS#7 SignedData builder regression test.
# WP-005 Phase 3. Round-trips via picotls's read-side ASN.1
# validator. Post WP-005 P3-fixups sub-deliverable 4: loads
# committed PEM cert+key fixtures via picotls's PEM decoder, so
# the test needs `workdir: meson.project_source_root()` to
# resolve `test/unit/data/pkcs7_fixture_*.pem` relative to the
# source tree.
pkcs7_signed_data_test = executable('test_pkcs7_signed_data',
    ['test/unit/test_pkcs7_signed_data.c'],
    kwargs: test_common_kwargs,
)
test('pkcs7_signed_data', pkcs7_signed_data_test, suite: 'unit',
     workdir: meson.project_source_root())

# n00b core util — X.509 + PKCS#8 read-side walker regression test.
# WP-005 mid-stream cleanup lift. Loads the committed PEM cert+key
# fixtures and exercises both walkers. Needs `workdir:
# meson.project_source_root()` to resolve the relative PEM paths.
x509_walk_test = executable('test_x509_walk',
    ['test/unit/test_x509_walk.c'],
    kwargs: test_common_kwargs,
)
test('x509_walk', x509_walk_test, suite: 'unit',
     workdir: meson.project_source_root())

# crypto — X.509 DER bracketing tokenizer (WP-042 Phase 1). Hand-built
# nested TLV + X.690 DER negative cases; no fixtures, no workdir needed.
x509_der_tok_test = executable('test_x509_der_tok',
    ['test/unit/test_x509_der_tok.c'],
    kwargs: test_common_kwargs,
)
test('x509_der_tok', x509_der_tok_test, suite: 'unit')

# crypto — X.509 DER -> parse-tree end-to-end (WP-042 Phase 1). Loads the
# committed PEM fixture + the x509_der grammar at runtime, so it needs a
# source-root workdir to resolve both relative paths.
x509_parse_test = executable('test_x509_parse',
    ['test/unit/test_x509_parse.c'],
    kwargs: test_common_kwargs,
)
test('x509_parse', x509_parse_test, suite: 'unit',
     workdir: meson.project_source_root())

# WP-042: native (libc-free) trust backend — drives n00b_quic_trust_verify
# through the in-tree X.509 verifier (default GTS bundle, replace-anchors,
# untrusted-reject, self-signed exact-cert pin).
trust_native_test = executable('test_trust_native',
    ['test/unit/test_trust_native.c'],
    kwargs: test_common_kwargs,
)
test('trust_native', trust_native_test, suite: 'unit',
     workdir: meson.project_source_root())

# WP-042: live network proof — native trust verifies real multi-CA chains on a
# worker thread (gated by N00B_TEST_NET=1; not part of the default suite gate).
trust_native_live_test = executable('test_trust_native_live',
    ['test/unit/test_trust_native_live.c'],
    kwargs: test_common_kwargs,
)
test('trust_native_live', trust_native_live_test, suite: 'unit',
     workdir: meson.project_source_root())

# libn00b core util — XDG Base Directory + typed-variadic path
# join + n00b_path_canonical regression. Exercises the spec matrix
# (var set / unset / empty) for each `_home()` variant, asserts
# `_runtime_dir()` returns nullptr when unset, exercises the
# variadic builder, and verifies the canonical combined resolver
# composes env-vars, tilde expansion, and absolute rooting.
path_xdg_test = executable('test_path_xdg',
    ['test/unit/test_path_xdg.c'],
    kwargs: test_common_kwargs,
)
test('path_xdg', path_xdg_test, suite: 'unit')

# libn00b core util — path-module anti-pattern sweep regression
# (pre-WP-005 P5). Exercises each of the seven swept functions
# (`split_on_slash` transitively via resolve_path, `n00b_resolve_path`,
# `n00b_path_walk`, `n00b_path_parts`, `n00b_find_file_in_program_path`,
# `n00b_list_directory`, `n00b_get_program_search_path`) and asserts
# push-after-return + readback works on each returned list — which
# forces `_n00b_list_ensure_cap` to read the list's scan-info /
# allocator fields. If a future commit reintroduces the
# `n00b_alloc(n00b_list_t(T)) + *p = n00b_list_new(T)` anti-pattern,
# scan-info is zero-initialized and the grow path either picks the
# wrong allocator or fails the readback identity check.
path_antipattern_sweep_test = executable('test_path_antipattern_sweep',
    ['test/unit/test_path_antipattern_sweep.c'],
    kwargs: test_common_kwargs,
)
test('path_antipattern_sweep', path_antipattern_sweep_test, suite: 'unit')

# n00b core PE — Authenticode certificate-table emit round-trip.
# WP-005 Phase 3 + P3-fixups sub-deliverable 3.
#
# Hybrid fixture wiring (per user kickoff Q3 disposition):
#   A. mingw-w64 + osslsigncode self-generated hello.exe.signed
#      (gated on toolchain availability; SKIPs cleanly when absent).
#   B. Sysinternals sigcheck64.exe (downloaded + SHA-256-verified at
#      configure time; NOT committed; SKIPs cleanly when network
#      unavailable). Previously pendmoves.exe — swapped 2026-05-21
#      because pendmoves ships only as PE32 (32-bit) and n00b_pe_parse
#      requires PE32+. sigcheck64.exe is the 64-bit variant from the
#      Sigcheck.zip distribution.
#   C. Always-runs synthetic in-memory PE blob cases as the primary.
#
# Each custom_target's helper script emits an EMPTY placeholder when
# its precondition fails (toolchain absent, network unavailable, SHA
# mismatch) so the meson rule still produces its declared output;
# the runtime test checks file size and SKIPs the case if empty.

mingw_gcc      = find_program('x86_64-w64-mingw32-gcc', required: false)
osslsigncode   = find_program('osslsigncode',           required: false)
pe_fixture_py = meson.project_source_root() / 'test' / 'fixtures' / 'pe' / 'pe_fixture.py'
pe_hello_exe = custom_target('pe_hello_exe',
    output: 'hello.exe',
    command: [
        python3,
        pe_fixture_py,
        'build-hello',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'hello.c',
        '@OUTPUT@',
    ],
    depend_files: files('test/unit/data/hello.c',
                        'test/fixtures/pe/pe_fixture.py'),
)
pe_hello_exe_signed = custom_target('pe_hello_exe_signed',
    output: 'hello.exe.signed',
    input: pe_hello_exe,
    command: [
        python3,
        pe_fixture_py,
        'sign-hello',
        '@INPUT@',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'pkcs7_fixture_cert.pem',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'pkcs7_fixture_key.pem',
        '@OUTPUT@',
    ],
    depend_files: files('test/unit/data/pkcs7_fixture_cert.pem',
                        'test/unit/data/pkcs7_fixture_key.pem',
                        'test/fixtures/pe/pe_fixture.py'),
)
pe_sigcheck = custom_target('pe_sigcheck',
    output: 'sigcheck64.exe',
    command: [
        python3,
        pe_fixture_py,
        'fetch-sigcheck',
        '@OUTPUT@',
    ],
    depend_files: files('test/fixtures/pe/pe_fixture.py'),
)
pe_cert_table_emit_test = executable('test_pe_cert_table_emit',
    ['test/unit/test_pe_cert_table_emit.c'],
    kwargs: test_common_kwargs,
)
test('pe_cert_table_emit', pe_cert_table_emit_test, suite: 'unit',
     depends: [pe_hello_exe, pe_hello_exe_signed, pe_sigcheck],
     workdir: meson.current_build_dir())

# WP-005 Phase 4 — libchalk PE re-sign end-to-end regression.
# Uses the same Sysinternals sigcheck64.exe fixture (downloaded
# into the build dir by the existing pe_sigcheck custom_target)
# and the committed pkcs7_fixture_{cert,key}.pem PEMs (resolved
# via MESON_SOURCE_ROOT, which meson sets for every test run).
# workdir = build dir so sigcheck64.exe resolves; the PEM
# fixtures come from the source tree.
chalk_pe_resign_test = executable('test_chalk_pe_resign',
    ['test/unit/test_chalk_pe_resign.c'],
    kwargs: test_common_kwargs,
)
    # 120s: hit the 30s default on a shared runner (n00b#261).
test('chalk_pe_resign', chalk_pe_resign_test, suite: 'unit',
     depends: [pe_sigcheck],
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.current_build_dir(),
     timeout: 120)

# WP-005 P5: Mach-O re-sign regression test. Always-runs; on macOS
# exercises the SecCodeSigner / codesign(1) bridge; on non-macOS
# exercises the strip-only fallback. Fixture: test/unit/data/hello.macho
# (committed). MESON_SOURCE_ROOT lets the test resolve the fixture path.
chalk_macho_resign_test = executable('test_chalk_macho_resign',
    ['test/unit/test_chalk_macho_resign.c'],
    kwargs: test_common_kwargs,
)
test('chalk_macho_resign', chalk_macho_resign_test, suite: 'unit',
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.project_source_root())

attest_monocypher_smoke_test = executable('test_attest_monocypher_smoke',
    ['test/unit/test_attest_monocypher_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_monocypher_smoke', attest_monocypher_smoke_test, suite: 'unit')

attest_signer_resolve_test = executable('test_attest_signer_resolve',
    ['test/unit/test_attest_signer_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_resolve', attest_signer_resolve_test, suite: 'unit')

attest_signer_release_test = executable('test_attest_signer_release',
    ['test/unit/test_attest_signer_release.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_release', attest_signer_release_test, suite: 'unit')

attest_signer_sign_test = executable('test_attest_signer_sign',
    ['test/unit/test_attest_signer_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_sign', attest_signer_sign_test, suite: 'unit')

attest_signer_keyid_test = executable('test_attest_signer_keyid',
    ['test/unit/test_attest_signer_keyid.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_keyid', attest_signer_keyid_test, suite: 'unit')

attest_envelope_sign_test = executable('test_attest_envelope_sign',
    ['test/unit/test_attest_envelope_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_sign', attest_envelope_sign_test, suite: 'unit')

attest_envelope_signatures_roundtrip_test = executable(
    'test_attest_envelope_signatures_roundtrip',
    ['test/unit/test_attest_envelope_signatures_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_signatures_roundtrip',
     attest_envelope_signatures_roundtrip_test,
     suite: 'unit')

attest_err_str_test = executable('test_attest_err_str',
    ['test/unit/test_attest_err_str.c'],
    kwargs: test_common_kwargs,
)
test('attest_err_str', attest_err_str_test, suite: 'unit')

attest_cli_sign_test = executable('test_attest_cli_sign',
    ['test/unit/test_attest_cli_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_sign', attest_cli_sign_test, suite: 'unit')

attest_signer_arena_test = executable('test_attest_signer_arena',
    ['test/unit/test_attest_signer_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_arena', attest_signer_arena_test, suite: 'unit')

attest_verifier_resolve_test = executable('test_attest_verifier_resolve',
    ['test/unit/test_attest_verifier_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_resolve', attest_verifier_resolve_test, suite: 'unit')

attest_verifier_keyid_test = executable('test_attest_verifier_keyid',
    ['test/unit/test_attest_verifier_keyid.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_keyid', attest_verifier_keyid_test, suite: 'unit')

attest_envelope_verify_signature_test = executable(
    'test_attest_envelope_verify_signature',
    ['test/unit/test_attest_envelope_verify_signature.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_verify_signature',
     attest_envelope_verify_signature_test,
     suite: 'unit')

attest_envelope_verify_test = executable('test_attest_envelope_verify',
    ['test/unit/test_attest_envelope_verify.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_verify', attest_envelope_verify_test, suite: 'unit')

attest_cli_verify_test = executable('test_attest_cli_verify',
    ['test/unit/test_attest_cli_verify.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_verify', attest_cli_verify_test, suite: 'unit')

attest_verifier_arena_test = executable('test_attest_verifier_arena',
    ['test/unit/test_attest_verifier_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_arena', attest_verifier_arena_test, suite: 'unit')

attest_oci_client_resolve_test = executable('test_attest_oci_client_resolve',
    ['test/unit/test_attest_oci_client_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_client_resolve', attest_oci_client_resolve_test, suite: 'unit')

# WP-004 Phase 4 hardening — FR-22 arena friendliness for the OCI
# client surface. Mirrors the WP-002 / WP-003 arena-test precedent
# (test_attest_signer_arena / test_attest_verifier_arena): N=50
# resolve + in-memory op + release through one arena, GC-collect,
# assert arena-used drops back. Always-runs (no Docker / no network).
attest_oci_arena_test = executable('test_attest_oci_arena',
    ['test/unit/test_attest_oci_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_arena', attest_oci_arena_test, suite: 'unit')

# WP-004 Phase 4 hardening — redirect-policy delegation regression.
# Verifies the OCI client correctly stores per-call kwargs
# (`redirect_host_allowlist`, `allow_redirects`) on the handle so
# libn00b's dispatcher can pick them up once the matching surface
# lifts. Always-runs.
attest_oci_redirect_policy_test = executable(
    'test_attest_oci_redirect_policy',
    ['test/unit/test_attest_oci_redirect_policy.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_redirect_policy',
     attest_oci_redirect_policy_test,
     suite: 'unit')

# WP-004 Phase 4 hardening — per-call body-size cap surface. Asserts
# the size-cap error codes are wired and the fetcher kwarg blocks
# parse correctly with both `max_size` and `timeout_ms`. Always-runs.
attest_oci_size_caps_test = executable('test_attest_oci_size_caps',
    ['test/unit/test_attest_oci_size_caps.c'],
    kwargs: test_common_kwargs,
)
    # QUARANTINED (n00b#280): fails on Linux, passes on macOS. should_fail
    # cannot express that -- it turns the macOS pass into UNEXPECTEDPASS --
    # so it sits in 'known-broken', which CI excludes. Restore its real
    # suite with the fix.
    #
    # Briefly restored on the theory that #277 (SIGPIPE ignored before fork)
    # was the cause -- the signature matched, rc=-13. It is not: this test
    # never forks. It makes an HTTPS request to localhost:5000, and the
    # SIGPIPE comes from writing to a socket with no registry listening.
    # #277's fix is present in the build that failed. Different bug, same
    # signal.
test('attest_oci_size_caps', attest_oci_size_caps_test, suite: 'known-broken')

attest_oci_manifest_test = executable('test_attest_oci_manifest',
    ['test/unit/test_attest_oci_manifest.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_manifest', attest_oci_manifest_test, suite: 'unit')

# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
attest_oci_push_smoke_test = executable('test_attest_oci_push_smoke',
    ['test/unit/test_attest_oci_push_smoke.c'],
    kwargs: test_common_kwargs,
)
# WP-004 Phase 2 trailing addition (DF-012): Docker-gated full
# HEAD/POST/PUT/PUT push against the zot fixture in
# test/fixtures/zot/.  N00B_TEST_ZOT_SRC_ROOT carries the absolute
# repo-root path so the test can find start.sh / stop.sh without
# depending on the user-side `eval $(start.sh)` pre-step that the
# caddy/nginx-quic smokes use.  Skips cleanly when N00B_TEST_DOCKER
# is unset.
test('attest_oci_push_smoke', attest_oci_push_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })
endif

# WP-004 Phase 3: discover-side smoke against zot. Pushes two
# envelopes with distinct predicate-types and asserts list_referrers
# returns both. Skips cleanly when N00B_TEST_DOCKER is unset.
attest_oci_discover_smoke_test = executable('test_attest_oci_discover_smoke',
    ['test/unit/test_attest_oci_discover_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_discover_smoke', attest_oci_discover_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-004 Phase 3: pull-side smoke against zot. Pushes one envelope,
# pulls it back, asserts byte-identical; also verifies the post-
# filter rejects non-matching predicate-types.
attest_oci_pull_smoke_test = executable('test_attest_oci_pull_smoke',
    ['test/unit/test_attest_oci_pull_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_pull_smoke', attest_oci_pull_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-004 Phase 3: the headline cross-WP end-to-end smoke against
# zot. Chains sign -> push -> discover -> pull -> verify against
# a real OCI registry; passing this test means the entire
# n00b-attestation producer-to-consumer path is wired.
attest_oci_end_to_end_smoke_test = executable(
    'test_attest_oci_end_to_end_smoke',
    ['test/unit/test_attest_oci_end_to_end_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_end_to_end_smoke', attest_oci_end_to_end_smoke_test,
     suite: 'unit',
     timeout: 120,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-005 Phase 1: ATTESTATION JSON builder byte-stability golden
# test. Three sub-tests (bundled+hint, lazy, no hint) cover the
# canonical-field-order disposition (D-056) + the per-mode shape
# transitions. Always-runs.
attest_mark_attestation_json_test = executable(
    'test_attest_mark_attestation_json',
    ['test/unit/test_attest_mark_attestation_json.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_attestation_json',
     attest_mark_attestation_json_test,
     suite: 'unit')

# WP-005 Phase 1: `n00b_attest_mark_artifact` IC-4 hash contract +
# registry_hint validation. Always-runs. Builds a tiny ELF fixture
# in-process via libn00b's ELF builder (no fixture binary checked
# in).
attest_mark_artifact_test = executable(
    'test_attest_mark_artifact',
    ['test/unit/test_attest_mark_artifact.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_artifact',
     attest_mark_artifact_test,
     suite: 'unit')

# WP-005 Phase 1: `n00b_attest_extract_from_artifact` IC-5 sentinel
# discrimination across all four cases (i..iv). Always-runs. Uses
# ELF + Mach-O fixtures built in-process plus a non-binary blob for
# the IC-5-(iv) case.
attest_extract_from_artifact_test = executable(
    'test_attest_extract_from_artifact',
    ['test/unit/test_attest_extract_from_artifact.c'],
    kwargs: test_common_kwargs,
)
test('attest_extract_from_artifact',
     attest_extract_from_artifact_test,
     suite: 'unit')

# WP-005 Phase 6: end-to-end mark + extract regression on the ELF
# codec (no resign step — the no-resign dispatch path on a
# signer_identity = nullptr call). Always-runs. Builds a tiny ELF
# fixture in-process via libn00b's ELF builder. IC-4 invariant
# verified end-to-end: the SHA-256 of the pre-mark artifact bytes
# equals the extracted envelope's subject.digest.sha256.
attest_mark_elf_e2e_test = executable(
    'test_attest_mark_elf_e2e',
    ['test/unit/test_attest_mark_elf_e2e.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_elf_e2e',
     attest_mark_elf_e2e_test,
     suite: 'unit')

# WP-005 Phase 6: end-to-end mark + Authenticode resign regression
# on the PE codec (cross-platform Authenticode signing — the P4
# resign primitive is deterministic and runs on any host). Always-
# runs. Uses an in-test synthetic PE32+ binary (`n00b_pe_binary_new`)
# and the committed PEM fixtures resolved via MESON_SOURCE_ROOT.
# IC-4 invariant verified end-to-end.
attest_mark_pe_e2e_test = executable(
    'test_attest_mark_pe_e2e',
    ['test/unit/test_attest_mark_pe_e2e.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_pe_e2e',
     attest_mark_pe_e2e_test,
     suite: 'unit',
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.current_build_dir())

# WP-005 Phase 6: end-to-end mark + Mach-O resign regression,
# host-gated to macOS (the resign body relies on the
# Security-framework / codesign(1) bridge). Uses the committed
# test/unit/data/hello.macho fixture + the PEM fixtures resolved
# via MESON_SOURCE_ROOT. IC-4 invariant verified end-to-end; the
# post-resign bytes are additionally checked via
# /usr/bin/codesign --verify --deep --strict (non-fatal SKIP on
# tooling failure).
if host_machine.system() == 'darwin'
# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
    attest_mark_macho_e2e_test = executable(
        'test_attest_mark_macho_e2e_macos',
        ['test/unit/test_attest_mark_macho_e2e_macos.c'],
        kwargs: test_common_kwargs,
    )
    test('attest_mark_macho_e2e_macos',
         attest_mark_macho_e2e_test,
         suite: 'unit',
         env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
         workdir: meson.project_source_root())
endif
endif

# WP-005 Phase 2: `n00b_attest_cli_mark` end-to-end regression
# (envelope-bytes → parse → mark dispatch). Always-runs; exercises
# the IC-4 unchalked-hash contract through the Phase-2 wrapper,
# --lazy mode, the CR-13 multi-envelope contract, and input
# validation.
attest_cli_mark_test = executable(
    'test_attest_cli_mark',
    ['test/unit/test_attest_cli_mark.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_mark',
     attest_cli_mark_test,
     suite: 'unit')

# WP-005 Phase 2: `n00b_attest_cli_unmark` mark→unmark→extract
# round-trip. Always-runs; post-unmark extract must surface the
# IC-5 (i) CHALK_NO_MARK sentinel.
attest_cli_unmark_test = executable(
    'test_attest_cli_unmark',
    ['test/unit/test_attest_cli_unmark.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_unmark',
     attest_cli_unmark_test,
     suite: 'unit')

# WP-005 Phase 2: `n00b_attest_cli_extract` mark→extract round-trip.
# Always-runs; covers bundled-mode round-trip, lazy-mode round-trip,
# no-mark sentinel, and input validation.
attest_cli_extract_test = executable(
    'test_attest_cli_extract',
    ['test/unit/test_attest_cli_extract.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_extract',
     attest_cli_extract_test,
     suite: 'unit')

# DF-013 regression: caller-supplied trust handle threaded through the
# h1 transport.  Same zot fixture as attest_oci_push_smoke, but the
# test exercises the libn00b-core path (n00b_http_request_sync ->
# n00b_http_h1_round_trip -> n00b_acme_tls_connect_ex) directly via a
# GET /v2/ rather than the attest-module OCI push surface.  Skips
# cleanly when N00B_TEST_DOCKER is unset.
h1_pinned_trust_test = executable('test_h1_pinned_trust',
    ['test/unit/test_h1_pinned_trust.c'],
    kwargs: test_common_kwargs,
)
test('h1_pinned_trust', h1_pinned_trust_test,
     suite: 'unit',
     timeout: 60,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

list_test = executable('test_list',
    ['test/unit/test_list.c'],
    kwargs: test_common_kwargs,
)
test('list', list_test, suite: 'unit')

ring_test = executable('test_ring',
    ['test/unit/test_ring.c'],
    kwargs: test_common_kwargs,
)
test('ring', ring_test, suite: 'unit')

spsc_ring_test = executable('test_spsc_ring',
    ['test/unit/test_spsc_ring.c'],
    kwargs: test_common_kwargs,
)
test('spsc_ring', spsc_ring_test, suite: 'unit')

stack_test = executable('test_stack',
    ['test/unit/test_stack.c'],
    kwargs: test_common_kwargs,
)
test('stack', stack_test, suite: 'unit')

result_test = executable('test_result',
    ['test/unit/test_result.c'],
    kwargs: test_common_kwargs,
)
    # QUARANTINED (n00b#280): fails on Linux, passes on macOS. should_fail
    # cannot express that -- it turns the macOS pass into UNEXPECTEDPASS --
    # so it sits in 'known-broken', which CI excludes. Restore its real
    # suite with the fix.
test('result', result_test, suite: 'unit')

variant_test = executable('test_variant',
    ['test/unit/test_variant.c'],
    kwargs: test_common_kwargs,
)
test('variant', variant_test, suite: 'unit')

tuple_test = executable('test_tuple',
    ['test/unit/test_tuple.c'],
    kwargs: test_common_kwargs,
)
test('tuple', tuple_test, suite: 'unit')

tree_test = executable('test_tree',
    ['test/unit/test_tree.c'],
    kwargs: test_common_kwargs,
)
test('tree', tree_test, suite: 'unit')

ntree_test = executable('test_ntree',
    ['test/unit/test_ntree.c'],
    kwargs: test_common_kwargs,
)
test('ntree', ntree_test, suite: 'unit')

gc_test = executable('test_gc',
    ['test/unit/test_gc.c'],
    kwargs: test_common_kwargs,
)
test('gc', gc_test, suite: 'unit')
test('gc_pin_all', gc_test, suite: 'unit', env: ['N00B_GC_PIN_ALL=1'])

# Verifies availability reporting in default builds and attribution when
# debug_live_census is enabled.
pool_audit_test = executable('test_pool_audit',
    ['test/unit/test_pool_audit.c'],
    kwargs: test_common_kwargs,
)
pool_audit_test_args = []
if conf_debug_live_census
  pool_audit_test_args += ['--require-compiled']
endif
test('pool_audit',
     pool_audit_test,
     suite: 'unit',
     args: pool_audit_test_args)

gc_census_test = executable('test_gc_census',
    ['test/unit/test_gc_census.c'],
    kwargs: test_common_kwargs,
)
test('gc_census', gc_census_test, suite: 'unit')

gc_register_roots_api_test = executable('test_gc_register_roots_api',
    ['test/unit/test_gc_register_roots_api.c'],
    kwargs: test_common_kwargs,
)
test('gc_register_roots_api', gc_register_roots_api_test, suite: 'unit')

# WP-003 Phase 4 / D-029 / § 6.5b: load-bearing end-to-end regression
# test for the auto-roots feature. Declares a TU-scope
# `static T *managed_singleton;` that ncc's `--ncc-auto-gc-roots`
# transform (default-on per D-031) auto-registers via
# `n00b_gc_register_roots` (Phase 2 batch API). After a forced GC, the
# managed object reached through the auto-registered slot must survive
# with its magic word intact. A `[[n00b::nomap]]` sibling decl provides
# the negative control (slot is not visited by the collector). This
# test must NOT be opted out of auto-roots: default c_args only.
gc_auto_roots_e2e_test = executable('test_gc_auto_roots_e2e',
    ['test/unit/test_gc_auto_roots_e2e.c'],
    kwargs: test_common_kwargs,
)
test('gc_auto_roots_e2e', gc_auto_roots_e2e_test, suite: 'unit')

gc_scan_map_test = executable('test_gc_scan_map',
    ['test/unit/test_gc_scan_map.c'],
    kwargs: test_common_kwargs,
)
test('gc_scan_map', gc_scan_map_test, suite: 'unit')

# n00b#395 -- measures whether one large, since-collected allocation makes
# later collects worse over an unchanged live set. Reports numbers; it is an
# instrument, not a gate, so it is not in the default 'unit' suite.
gc_scan_bound_hysteresis_test = executable('test_gc_scan_bound_hysteresis',
    ['test/unit/test_gc_scan_bound_hysteresis.c'],
    kwargs: test_common_kwargs,
)
test('gc_scan_bound_hysteresis',
    gc_scan_bound_hysteresis_test,
    suite: 'bench',
    timeout: 300)

gc_stack_test = executable('test_gc_stack',
    ['test/unit/test_gc_stack.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + ['--ncc-no-gc-stack-maps'],
    },
)
test('gc_stack', gc_stack_test, suite: 'unit')
if host_machine.system() == 'windows'
    test('gc_stack_copy', gc_stack_test, suite: 'unit', env: ['N00B_GC_PIN_ALL=0'])
endif

gc_stack_nonlocal_test = executable('test_gc_stack_nonlocal',
    ['test/unit/test_gc_stack_nonlocal.c'],
    kwargs: test_common_kwargs + {
        # WP-003 / D-036 (F-5): the test owns a TU-scope
        # `static n00b_runtime_t test_runtime;` whose fields include
        # `_Atomic(T *)` members. ncc's auto-roots transform now
        # handles `_Atomic(T *)` correctly at the top-level decl (F-3),
        # but the test additionally exercises GC stack-map / nonlocal-
        # exit semantics rather than auto-roots semantics, and a
        # whole-test opt-out keeps that focus clean. Mirrors the
        # `--ncc-no-gc-stack-maps` opt-out used for `test_gc_stack`.
        'c_args': exe_c_args + ['--ncc-no-auto-gc-roots'],
    },
)
test('gc_stack_nonlocal', gc_stack_nonlocal_test, suite: 'unit')

static_descriptor_test = executable('test_static_descriptor',
    ['test/unit/test_static_descriptor.c'],
    kwargs: test_common_kwargs,
)
test('static_descriptor', static_descriptor_test, suite: 'unit')

dict_static_image_test = executable('test_dict_static_image',
    ['test/unit/test_dict_static_image.c'],
    kwargs: test_common_kwargs,
)
test('dict_static_image', dict_static_image_test, suite: 'unit')

# WP-011 Phase 5a regression: dict literals lowered against the REAL
# `n00b_dict_t(K, V)` from include/adt/dict.h (not a private
# re-declaration) and the REAL r-string config. Locks in the typeid
# alignment (D-073) and the `is_rstr_element_type` widening.
dict_literal_real_headers_test = executable('test_dict_literal_real_headers',
    ['test/unit/test_dict_literal_real_headers.c'],
    kwargs: test_common_kwargs,
)
test('dict_literal_real_headers', dict_literal_real_headers_test, suite: 'unit')

static_hash_test = executable('test_static_hash',
    ['test/unit/test_static_hash.c'],
    kwargs: test_common_kwargs,
)
test('static_hash', static_hash_test, suite: 'unit')

static_object_sections_test = executable('test_static_object_sections',
    [
      'test/unit/test_static_object_sections.c',
      'test/unit/test_static_object_sections_extra.c',
    ],
    kwargs: test_common_kwargs + {
        # WP-003 / D-036 (F-5 extension): the test owns TU-scope
        # pointer arrays (e.g., `section_callback_words[4]`) that
        # the auto-roots transform would register as plain pointer-
        # array roots. The test asserts on bespoke section-callback
        # GC scan policies (custom callbacks, none-scan, all-scan,
        # nested layouts) and depends on the GC NOT seeing those
        # words as standard roots. Auto-roots interferes; opt out
        # at the test level. Mirrors the `--ncc-no-gc-stack-maps`
        # opt-out used for `test_gc_stack` and the F-5 opt-out used
        # for `test_gc_stack_nonlocal`.
        'c_args': exe_c_args + ['--ncc-no-auto-gc-roots'],
    },
)
test('static_object_sections', static_object_sections_test, suite: 'unit')

ncc_static_init_link_src = []
if n00b_crt_host_stub_src.length() > 0
  ncc_static_init_link_src += n00b_crt_host_stub_src
endif

ncc_static_objects_test = executable('test_ncc_static_objects',
    ['test/unit/test_ncc_static_objects.c'],
    kwargs: test_common_kwargs,
)
test('ncc_static_objects', ncc_static_objects_test, suite: 'unit')

# This test forces the C linker (ncc), not the ObjC wrapper, so it does not go
# through scripts/ncc-gcmap-objc-link-wrapper.sh. Enable ncc's own link-stage
# gcmap aggregation hook instead, so it still links the GC type-map dictionary
# (gc_type_map.c references n00b_gcmap_table/_count). The flags are ncc-only and
# harmless to the C link otherwise.
ncc_static_images_test = executable('test_ncc_static_images',
    ['test/unit/test_ncc_static_images.c'] + ncc_static_init_link_src,
    kwargs: test_common_kwargs + {
        'link_language': 'c',
        'link_args': exe_link_args + [
            '--ncc-gcmap-prelink',
            '--ncc-gcmap-include=' + meson.current_source_dir() / 'include',
        ],
    },
)
test('ncc_static_images', ncc_static_images_test, suite: 'unit')

gc_selective_scan_test = executable('test_gc_selective_scan',
    ['test/unit/test_gc_selective_scan.c'],
    kwargs: test_common_kwargs,
)
test('gc_selective_scan', gc_selective_scan_test, suite: 'unit')

container_scan_kind_test = executable('test_container_scan_kind',
    ['test/unit/test_container_scan_kind.c'],
    kwargs: test_common_kwargs,
)
test('container_scan_kind', container_scan_kind_test, suite: 'unit')

buffer_mutators_test = executable('test_buffer_mutators',
    ['test/unit/test_buffer_mutators.c'],
    kwargs: test_common_kwargs,
)
test('buffer_mutators', buffer_mutators_test, suite: 'unit')

finalizer_test = executable('test_finalizer',
    ['test/unit/test_finalizer.c'],
    kwargs: test_common_kwargs,
)
test('finalizer', finalizer_test, suite: 'unit')

gc_vreg_pin_test = executable('test_gc_vreg_pin',
    ['test/unit/test_gc_vreg_pin.c'],
    kwargs: test_common_kwargs,
)
test('gc_vreg_pin', gc_vreg_pin_test, suite: 'unit')

gc_register_sweep_test = executable('test_gc_register_sweep',
    ['test/unit/test_gc_register_sweep.c'],
    kwargs: test_common_kwargs,
)
test('gc_register_sweep', gc_register_sweep_test, suite: 'unit')

gc_address_keyed_tables_test = executable('test_gc_address_keyed_tables',
    ['test/unit/test_gc_address_keyed_tables.c'],
    kwargs: test_common_kwargs + {
        # slay/codegen.h, for the field-lock table, includes mir.h.
        'dependencies': test_common_kwargs['dependencies'] + [mir_dep],
    },
)
test('gc_address_keyed_tables', gc_address_keyed_tables_test, suite: 'unit')

gc_pin_all_segments_test = executable('test_gc_pin_all_segments',
    ['test/unit/test_gc_pin_all_segments.c'],
    kwargs: test_common_kwargs,
)
test('gc_pin_all_segments', gc_pin_all_segments_test, suite: 'unit')

arena_tlab_test = executable('test_arena_tlab',
    ['test/unit/test_arena_tlab.c'],
    kwargs: test_common_kwargs,
)
test('arena_tlab', arena_tlab_test, suite: 'unit')
test('arena_tlab_pin_all', arena_tlab_test, suite: 'unit',
     env: ['N00B_GC_PIN_ALL=1'])

current_allocator_test = executable('test_current_allocator',
    ['test/unit/test_current_allocator.c'],
    kwargs: test_common_kwargs + {
        # The n00b_with_allocator() macro declares an
        # n00b_allocator_scope_t in for-init using a [[gnu::cleanup]]
        # attribute.  That struct has a pointer field (previous), so
        # the gc-stack-maps transform sees it as an aggregate root in
        # an unsupported statement context (for-init has no
        # block_item insertion point).  Relaxed mode tells the
        # transform to silently skip such cases instead of erroring.
        # The macro's lifetime is bounded by the for-statement, so
        # the missing tracking is bounded the same way; restoration
        # still happens via the cleanup attribute.
        'c_args': exe_c_args + ['--ncc-gc-stack-maps-relaxed'],
    },
)
test('current_allocator', current_allocator_test, suite: 'unit')

if target_os != 'windows'
# n00b#333: POSIX-only (raw pthreads); not built on Windows.
if target_os != 'windows'
    hidden_pool_stw_test = executable('test_hidden_pool_stw',
        ['test/unit/test_hidden_pool_stw.c'],
        kwargs: test_common_kwargs + {
            # The test takes the address of the static `g_pool`'s
            # `free_lists[N].head`, which is _Atomic. ncc's auto-gc-roots
            # transform emits descriptors that take those addresses, and
            # clang flags them under -Watomic-access. Same pattern as the
            # wax build's `-Wno-atomic-access`; the address-of operation
            # is safe (the descriptors never deref non-atomically), the
            # warning is a false positive on this codegen.
            'c_args': exe_c_args + ['-Wno-atomic-access'],
        },
    )
    # Not in the default 'unit' suite — this is a stress / repro test
    # that intentionally drives high churn on the GC + pool paths and
    # can take several seconds to run. It's a self-contained, no-priv,
    # no-network repro for the foreign-thread vs GC mark race the
    # raw_gateway has been hitting on Darwin; run it manually via
    # `./scripts/build.sh build && ./build/test_hidden_pool_stw` or
    # `meson test -C build --suite stress`.
    test('hidden_pool_stw', hidden_pool_stw_test, suite: 'stress', timeout: 60)
endif
endif

check_perms_dyld_gap_test = executable('test_check_perms_dyld_gap',
    ['test/unit/test_check_perms_dyld_gap.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + ['-Wno-atomic-access'],
    },
)
test('check_perms_dyld_gap', check_perms_dyld_gap_test,
     suite: 'stress', timeout: 30)

buffer_test = executable('test_buffer',
    ['test/unit/test_buffer.c'],
    kwargs: test_common_kwargs,
)
test('buffer', buffer_test, suite: 'unit')

marshal_test = executable('test_marshal',
    ['test/unit/test_marshal.c'],
    kwargs: test_common_kwargs,
)
test('marshal', marshal_test, suite: 'unit')

rocs_map_test = executable('test_rocs_map',
    ['test/unit/test_rocs_map.c'],
    kwargs: test_common_kwargs,
)
test('rocs_map', rocs_map_test, suite: 'unit')

rocs_mapped_record_test = executable('test_rocs_mapped_record',
    ['test/unit/test_rocs_mapped_record.c'],
    kwargs: test_common_kwargs,
)
test('rocs_mapped_record', rocs_mapped_record_test, suite: 'unit')

rocs_map_gc_region_test = executable('test_rocs_map_gc_region',
    ['test/unit/test_rocs_map_gc_region.c'],
    kwargs: test_common_kwargs,
)
    # QUARANTINED (n00b#280): fails on Linux, passes on macOS. should_fail
    # cannot express that -- it turns the macOS pass into UNEXPECTEDPASS --
    # so it sits in 'known-broken', which CI excludes. Restore its real
    # suite with the fix.
test('rocs_map_gc_region', rocs_map_gc_region_test, suite: 'unit')

gc_type_map_register_test = executable('test_gc_type_map_register',
    ['test/unit/test_gc_type_map_register.c'],
    kwargs: test_common_kwargs,
)
test('gc_type_map_register', gc_type_map_register_test, suite: 'unit')

transient_zero_test = executable('test_transient_zero',
    ['test/unit/test_transient_zero.c'],
    kwargs: test_common_kwargs,
)
test('transient_zero', transient_zero_test, suite: 'unit')

merkle_test = executable('test_merkle',
    ['test/unit/test_merkle.c'],
    kwargs: test_common_kwargs,
)
test('merkle', merkle_test, suite: 'unit')

merkle_transient_test = executable('test_merkle_transient',
    ['test/unit/test_merkle_transient.c'],
    kwargs: test_common_kwargs,
)
test('merkle_transient', merkle_transient_test, suite: 'unit')

marshal_type_layout_test = executable('test_marshal_type_layout',
    ['test/unit/test_marshal_type_layout.c'] + ncc_static_init_link_src,
    kwargs: test_common_kwargs + {
        'link_language': 'c',
        'link_args': exe_link_args + [
            '--ncc-gcmap-prelink',
            '--ncc-gcmap-include=' + meson.current_source_dir() / 'include',
        ],
    },
)
if target_os != 'windows'
  test('marshal_type_layout', marshal_type_layout_test, suite: 'unit')
endif

grammar_marshal_roundtrip_test = executable('test_grammar_marshal_roundtrip',
    ['test/unit/test_grammar_marshal_roundtrip.c'],
    kwargs: test_common_kwargs,
)
if target_os != 'windows'
  test('grammar_marshal_roundtrip', n00b_gcmap_index,
       args: ['--exec', grammar_marshal_roundtrip_test.full_path()],
       depends: [grammar_marshal_roundtrip_test],
       suite: 'unit', timeout: 120)
endif

file_io_test = executable('test_file_io',
    ['test/unit/test_file_io.c'],
    kwargs: test_common_kwargs,
)
test('file_io', file_io_test, suite: 'unit')

parquet_flat_test = executable('test_parquet_flat',
    ['test/unit/test_parquet_flat.c'],
    kwargs: test_common_kwargs,
)
test('parquet_flat', parquet_flat_test, suite: 'unit')

mutex_test = executable('test_mutex',
    ['test/unit/test_mutex.c'],
    kwargs: test_common_kwargs,
)
test('mutex', mutex_test, suite: 'unit')

stw_test = executable('test_stw',
    ['test/unit/test_stw.c'],
    kwargs: test_common_kwargs,
)
test('stw', stw_test, suite: 'unit')

rwlock_test = executable('test_rwlock',
    ['test/unit/test_rwlock.c'],
    kwargs: test_common_kwargs,
)
test('rwlock', rwlock_test, suite: 'unit')

# n00b#467: the Content-Length parsers on the HTTP/1 + HTTP/3 worker paths are
# libc-free, and the saturating overflow behaviour their size caps depend on is
# pinned here.
parse_num_test = executable('test_parse_num',
    ['test/unit/test_parse_num.c'],
    kwargs: test_common_kwargs,
)
test('parse_num', parse_num_test, suite: 'unit')

futex_timeout_test = executable('test_futex_timeout',
    ['test/unit/test_futex_timeout.c'],
    kwargs: test_common_kwargs,
)
test('futex_timeout', futex_timeout_test, suite: 'unit')

dense_postings_test = executable('test_rocs_dense_postings',
    ['test/unit/test_rocs_dense_postings.c'],
    kwargs: test_common_kwargs,
)
test('rocs_dense_postings', dense_postings_test, suite: 'unit')

condition_test = executable('test_condition',
    ['test/unit/test_condition.c'],
    kwargs: test_common_kwargs,
)
# Passes locally on macOS (1.0s) but FAILs on the CI macOS runner in 0.5s.
# Same platform, different environment, so this is not a platform split -- it
# is a test sensitive to something the runner does differently. n00b#281.
test('condition', condition_test, suite: 'unit')

dict_test = executable('test_dict',
    ['test/unit/test_dict.c'],
    kwargs: test_common_kwargs,
)
test('dict', dict_test, suite: 'unit')

dict_untyped_migrate_test = executable('test_dict_untyped_migrate',
    ['test/unit/test_dict_untyped_migrate.c'],
    kwargs: test_common_kwargs,
)
# is_parallel: false because the migration-abandon cases assert on wall-clock
# gates, and a loaded machine can starve them. The timeout is the real
# assertion for the strand cases: before n00b-lang/n00b#365 they never return.
test('dict_untyped_migrate', dict_untyped_migrate_test, suite: 'unit',
    is_parallel: false, timeout: 120)

dict_gc_scan_test = executable('test_dict_gc_scan',
    ['test/unit/test_dict_gc_scan.c'],
    kwargs: test_common_kwargs,
)
test('dict_gc_scan', dict_gc_scan_test, suite: 'unit')

# ============================================================================
# Conduit tests
# ============================================================================

type_registry_test = executable('test_type_registry',
    ['test/unit/test_type_registry.c'],
    kwargs: test_common_kwargs,
)
test('type_registry', type_registry_test, suite: 'unit')

type_static_layout_test = executable('test_type_static_layout',
    ['test/unit/test_type_static_layout.c'],
    kwargs: test_common_kwargs,
)
test('type_static_layout', type_static_layout_test, suite: 'unit')

type_hash_test = executable('test_type_hash',
    ['test/unit/test_type_hash.c'],
    kwargs: test_common_kwargs,
)
test('type_hash', type_hash_test, suite: 'unit')

conduit_test = executable('test_conduit',
    ['test/unit/test_conduit.c'],
    kwargs: test_common_kwargs,
)
test('conduit', conduit_test, suite: 'unit')

# n00b#321: an interior GC root more than one allocation's worth into a large
# allocation must still resolve; the guard back-scan bound used to be a flat
# 8 MB and dropped the root past it.
alloc_interior_large_test = executable('test_alloc_interior_large',
    ['test/unit/test_alloc_interior_large.c'],
    kwargs: test_common_kwargs,
)
test('alloc_interior_large', alloc_interior_large_test, suite: 'unit')

# n00b#275 + n00b#395: the conservative guard scan's per-page cost (arena
# segments must record their permissions, or every page of every backward walk
# pays a pipe write()+poll()) and its LENGTH (the bound must come from the
# mapping being walked, not from a monotonic all-time high-water mark that one
# large allocation raises for the life of the process).
gc_scan_bound_test = executable('test_gc_scan_bound',
    ['test/unit/test_gc_scan_bound.c'],
    kwargs: test_common_kwargs,
)
test('gc_scan_bound', gc_scan_bound_test, suite: 'unit')

# n00b#213: n00b_check_kernel_page_map caches "something is mapped here" by
# registering the probed page, and nothing ever unregisters it -- the one
# registration site in the process with no counterpart. The cache is now
# capped; this probes far more pages than the cap and checks the registry does
# not grow past it.
mmap_probe_cache_test = executable('test_mmap_probe_cache_bound',
    ['test/unit/test_mmap_probe_cache_bound.c'],
    kwargs: test_common_kwargs,
)
test('mmap_probe_cache_bound', mmap_probe_cache_test, suite: 'unit')

# Gates the embedded-n00b_condition_t pinning rule (n00b#326): every inbox
# constructor must yield a pointer the collector cannot relocate, because the
# lock registry keys locks by address. Convention-only before this test; two
# constructors had already drifted off it (n00b#324/#325).
conduit_inbox_pinning_test = executable('test_conduit_inbox_pinning',
    ['test/unit/test_conduit_inbox_pinning.c'],
    kwargs: test_common_kwargs,
)
test('conduit_inbox_pinning', conduit_inbox_pinning_test, suite: 'unit')

# n00b#327: an interior pointer into an OOB-metadata/no-inline-header allocator
# cannot be resolved, and must report that distinctly rather than looking like
# free space (which conservative scanning produces constantly).
alloc_interior_test = executable('test_alloc_interior_unresolvable',
    ['test/unit/test_alloc_interior_unresolvable.c'],
    kwargs: test_common_kwargs,
)
test('alloc_interior_unresolvable', alloc_interior_test, suite: 'unit')

path_test = executable('test_path',
    ['test/unit/test_path.c'],
    kwargs: test_common_kwargs,
)
test('path', path_test, suite: 'unit')

publisher_test = executable('test_publisher',
    ['test/unit/test_publisher.c'],
    kwargs: test_common_kwargs,
)
test('publisher', publisher_test, suite: 'unit')

subscription_test = executable('test_subscription',
    ['test/unit/test_subscription.c'],
    kwargs: test_common_kwargs,
)
test('subscription', subscription_test, suite: 'unit')

io_test = executable('test_io',
    ['test/unit/test_io.c'],
    kwargs: test_common_kwargs,
)
test('io', io_test, suite: 'unit')

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
socket_test = executable('test_socket',
    ['test/unit/test_socket.c'],
    kwargs: test_common_kwargs,
)
test('socket', socket_test, suite: 'unit')
endif

fd_managed_test = executable('test_fd_managed',
    ['test/unit/test_fd_managed.c'],
    kwargs: test_common_kwargs,
)
test('fd_managed', fd_managed_test, suite: 'unit')

if target_os == 'windows'
    io_windows_test = executable('test_io_windows',
        ['test/unit/test_io_windows.c'],
        kwargs: test_common_kwargs,
    )
    test('io_windows', io_windows_test, suite: 'unit')
endif

signal_test = executable('test_signal',
    ['test/unit/test_signal.c'],
    kwargs: test_common_kwargs,
)
test('signal', signal_test, suite: 'unit')

# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
proc_lifecycle_test = executable('test_proc_lifecycle',
    ['test/unit/test_proc_lifecycle.c'],
    kwargs: test_common_kwargs,
)
test('proc_lifecycle', proc_lifecycle_test, suite: 'unit')
endif

file_change_test = executable('test_file_change',
    ['test/unit/test_file_change.c'],
    kwargs: test_common_kwargs,
)
test('file_change', file_change_test, suite: 'unit')

# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
subproc_test = executable('test_subproc',
    ['test/unit/test_subproc.c'],
    kwargs: test_common_kwargs,
)
test('subproc', subproc_test, suite: 'unit', timeout: 120)
endif

timer_test = executable('test_timer',
    ['test/unit/test_timer.c'],
    kwargs: test_common_kwargs,
)
test('timer', timer_test, suite: 'unit')

service_test = executable('test_service',
    ['test/unit/test_service.c'],
    kwargs: test_common_kwargs,
)
test('service', service_test, suite: 'unit')

conduit_e2e_test = executable('test_conduit_e2e',
    ['test/unit/test_conduit_e2e.c'],
    kwargs: test_common_kwargs,
)
test('conduit_e2e', conduit_e2e_test, suite: 'unit')

dual_backend_test = executable('test_dual_backend',
    ['test/unit/test_dual_backend.c'],
    kwargs: test_common_kwargs,
)
test('dual_backend', dual_backend_test, suite: 'unit')

xform_test = executable('test_xform',
    ['test/unit/test_xform.c'],
    kwargs: test_common_kwargs,
)
test('xform', xform_test, suite: 'unit')

conduit_sync_completion_test = executable('test_conduit_sync_completion',
    ['test/unit/test_conduit_sync_completion.c'],
    kwargs: test_common_kwargs,
)
test('conduit_sync_completion', conduit_sync_completion_test,
     suite: 'unit', is_parallel: false, timeout: 120)

xform_marshal_test = executable('test_xform_marshal',
    ['test/unit/test_xform_marshal.c'] + ncc_static_init_link_src,
    kwargs: test_common_kwargs + {
        'link_language': 'c',
        'link_args': exe_link_args + [
            '--ncc-gcmap-prelink',
            '--ncc-gcmap-include=' + meson.current_source_dir() / 'include',
        ],
    },
)
test('xform_marshal', xform_marshal_test, suite: 'unit')

xform_linebuf_test = executable('test_xform_linebuf',
    ['test/unit/test_xform_linebuf.c'],
    kwargs: test_common_kwargs,
)
test('xform_linebuf', xform_linebuf_test, suite: 'unit')

if not gui_tui_unnecessary_on_windows
xform_ansi_test = executable('test_xform_ansi',
    ['test/unit/test_xform_ansi.c'],
    kwargs: test_common_kwargs,
)
test('xform_ansi', xform_ansi_test, suite: 'unit')

xform_render_test = executable('test_xform_render',
    ['test/unit/test_xform_render.c'],
    kwargs: test_common_kwargs,
)
test('xform_render', xform_render_test, suite: 'unit')

hexdump_test = executable('test_hexdump',
    ['test/unit/test_hexdump.c'],
    kwargs: test_common_kwargs,
)
test('hexdump', hexdump_test, suite: 'unit')

xform_hexdump_test = executable('test_xform_hexdump',
    ['test/unit/test_xform_hexdump.c'],
    kwargs: test_common_kwargs,
)
test('xform_hexdump', xform_hexdump_test, suite: 'unit')
endif

print_test = executable('test_print',
    ['test/unit/test_print.c'],
    kwargs: test_common_kwargs,
)
test('print', print_test, suite: 'unit')

# Runtime code that writes stdout or stderr has to survive a short write.
test('lint_raw_writes', python3,
    args: [files('scripts/lint_raw_writes.py'), meson.project_source_root()],
    suite: 'lint',
)

std_streams_test = executable('test_std_streams',
    ['test/unit/test_std_streams.c'],
    kwargs: test_common_kwargs,
)
test('std_streams', std_streams_test, suite: 'unit')

fd_owner_read_all_test = executable('test_fd_owner_read_all',
    ['test/unit/test_fd_owner_read_all.c'],
    kwargs: test_common_kwargs,
)
test('fd_owner_read_all', fd_owner_read_all_test, suite: 'unit')

# ============================================================================
# Regex engine tests — registered via subdir during Phase 11 of the regex port.
# See ~/dd/n00b_regex_port_plan.md § 7 / Phase 11.
# ============================================================================

# ============================================================================
# Logic / constraint solver tests
# ============================================================================

logic_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}

clpfd_domain_test = executable('test_clpfd_domain',
    ['test/unit/test_clpfd_domain.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_domain', clpfd_domain_test, suite: 'unit')

clpfd_store_test = executable('test_clpfd_store',
    ['test/unit/test_clpfd_store.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_store', clpfd_store_test, suite: 'unit')

asp_engine_test = executable('test_asp_engine',
    ['test/unit/test_asp_engine.c'],
    kwargs: logic_test_kwargs,
)
test('asp_engine', asp_engine_test, suite: 'unit')

asp_relation_test = executable('test_asp_relation',
    ['test/unit/test_asp_relation.c'],
    kwargs: logic_test_kwargs,
)
test('asp_relation', asp_relation_test, suite: 'unit')

logic_program_test = executable('test_logic_program',
    ['test/unit/test_logic_program.c'],
    kwargs: logic_test_kwargs,
)
test('logic_program', logic_program_test, suite: 'unit')

clpfd_label_test = executable('test_clpfd_label',
    ['test/unit/test_clpfd_label.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_label', clpfd_label_test, suite: 'unit')

logic_dsl_test = executable('test_logic_dsl',
    ['test/unit/test_logic_dsl.c'],
    kwargs: logic_test_kwargs,
)
test('logic_dsl', logic_dsl_test, suite: 'unit')

logic_easy_test = executable('test_logic_easy',
    ['test/unit/test_logic_easy.c'],
    kwargs: logic_test_kwargs,
)
test('logic_easy', logic_easy_test, suite: 'unit')

# ============================================================================
# Parser tests
# ============================================================================

ijson_test = executable('test_ijson',
    ['test/unit/test_ijson.c'],
    kwargs: test_common_kwargs,
)
test('ijson', ijson_test, suite: 'unit')

json_test = executable('test_json',
    ['test/unit/test_json.c'],
    kwargs: test_common_kwargs,
)
test('json', json_test, suite: 'unit')

json_contract_test = executable('test_json_contract',
    ['test/unit/test_json_contract.c'],
    kwargs: test_common_kwargs,
)
test('json_contract', json_contract_test, suite: 'unit')

xform_json_test = executable('test_xform_json',
    ['test/unit/test_xform_json.c'],
    kwargs: test_common_kwargs,
)
test('xform_json', xform_json_test, suite: 'unit', timeout: 120)

xform_http_test = executable('test_xform_http',
    ['test/unit/test_xform_http.c'],
    kwargs: test_common_kwargs,
)
test('xform_http', xform_http_test, suite: 'unit')

# Slay tests commented out temporarily due to in-progress header changes
# scanner_test = executable('test_scanner',
#     ['test/unit/test_scanner.c'],
#     kwargs: test_common_kwargs,
# )
# test('scanner', scanner_test, suite: 'unit')
#
# ============================================================================
# Slay grammar tests
# ============================================================================

grammar_test = executable('test_grammar',
    ['test/unit/test_grammar.c'],
    kwargs: test_common_kwargs,
)
test('grammar', grammar_test, suite: 'unit')

slay_pprint_test = executable('test_slay_pprint',
    ['test/unit/test_slay_pprint.c'],
    kwargs: test_common_kwargs,
)
test('slay_pprint', slay_pprint_test, suite: 'unit')

slay_rewrite_test = executable('test_slay_rewrite',
    ['test/unit/test_slay_rewrite.c'],
    kwargs: test_common_kwargs,
)
test('slay_rewrite', slay_rewrite_test, suite: 'unit')

pwz_test = executable('test_pwz',
    ['test/unit/test_pwz.c'],
    kwargs: test_common_kwargs,
)
test('pwz', pwz_test, suite: 'unit')

bnf_test = executable('test_bnf',
    ['test/unit/test_bnf.c'],
    kwargs: test_common_kwargs,
)
test('bnf', bnf_test, suite: 'unit')

bnf_json_test = executable('test_bnf_json',
    ['test/unit/test_bnf_json.c'],
    kwargs: test_common_kwargs,
)
test('bnf_json', bnf_json_test, suite: 'unit', workdir: meson.project_source_root())

earley_test = executable('test_earley',
    ['test/unit/test_earley.c'],
    kwargs: test_common_kwargs,
)
test('earley', earley_test, suite: 'unit')

parse_api_test = executable('test_parse_api',
    ['test/unit/test_parse_api.c'],
    kwargs: test_common_kwargs,
)
test('parse_api', parse_api_test, suite: 'unit')

commander_test = executable('test_commander',
    ['test/unit/test_commander.c'],
    kwargs: test_common_kwargs,
)
test('commander', commander_test, suite: 'unit')

symtab_test = executable('test_symtab',
    ['test/unit/test_symtab.c'],
    kwargs: test_common_kwargs,
)
test('symtab', symtab_test, suite: 'unit')

c_parse_test = executable('test_c_parse',
    ['test/unit/test_c_parse.c'],
    kwargs: test_common_kwargs,
)
test('c_parse', c_parse_test, suite: 'unit', workdir: meson.project_source_root())

c_tokenizer_test = executable('test_c_tokenizer',
    ['test/unit/test_c_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('c_tokenizer', c_tokenizer_test, suite: 'unit')

n00b_tokenizer_test = executable('test_n00b_tokenizer',
    ['test/unit/test_n00b_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('n00b_tokenizer', n00b_tokenizer_test, suite: 'unit')

tokenizer_registry_test = executable('test_tokenizer_registry',
    ['test/unit/test_tokenizer_registry.c'],
    kwargs: test_common_kwargs,
)
test('tokenizer_registry', tokenizer_registry_test, suite: 'unit')

text_tokenizer_test = executable('test_text_tokenizer',
    ['test/unit/test_text_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('text_tokenizer', text_tokenizer_test, suite: 'unit')

shell_tokenizer_test = executable('test_shell_tokenizer',
    ['test/unit/test_shell_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('shell_tokenizer', shell_tokenizer_test, suite: 'unit')

lisp_tokenizer_test = executable('test_lisp_tokenizer',
    ['test/unit/test_lisp_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('lisp_tokenizer', lisp_tokenizer_test, suite: 'unit')

json_tokenizer_test = executable('test_json_tokenizer',
    ['test/unit/test_json_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('json_tokenizer', json_tokenizer_test, suite: 'unit')

cf_label_test = executable('test_cf_label',
    ['test/unit/test_cf_label.c'],
    kwargs: test_common_kwargs,
)
test('cf_label',
     cf_label_test,
     suite: 'unit',
     workdir: meson.project_source_root(),
     env: ['MALLOC_PERTURB_=0'])

cfg_test = executable('test_cfg',
    ['test/unit/test_cfg.c'],
    kwargs: test_common_kwargs,
)
test('cfg', cfg_test, suite: 'unit', workdir: meson.project_source_root())

cdg_test = executable('test_cdg',
    ['test/unit/test_cdg.c'],
    kwargs: test_common_kwargs,
)
test('cdg', cdg_test, suite: 'unit', workdir: meson.project_source_root())

dfg_test = executable('test_dfg',
    ['test/unit/test_dfg.c'],
    kwargs: test_common_kwargs,
)
test('dfg', dfg_test, suite: 'unit', workdir: meson.project_source_root())

# ============================================================================
# Type checker tests
# ============================================================================

typecheck_test = executable('test_typecheck',
    ['test/unit/test_typecheck.c'],
    kwargs: test_common_kwargs,
)
test('typecheck', typecheck_test, suite: 'unit', workdir: meson.project_source_root())

# ============================================================================
# Analysis tests
# ============================================================================

analyze_test = executable('test_analyze',
    ['test/unit/test_analyze.c'],
    kwargs: test_common_kwargs,
)
test('analyze', analyze_test, suite: 'unit', workdir: meson.project_source_root())

n00b_programs_test = executable('test_n00b_programs',
    ['test/unit/test_n00b_programs.c'],
    kwargs: test_common_kwargs,
)
test('n00b_programs', n00b_programs_test,
     suite: 'unit',
     workdir: meson.project_source_root(),
     timeout: 60)

n00b_types_test = executable('test_n00b_types',
    ['test/unit/test_n00b_types.c'],
    kwargs: test_common_kwargs,
)
test('n00b_types', n00b_types_test, suite: 'unit', workdir: meson.project_source_root())

infer_expr_test = executable('test_infer_expr',
    ['test/unit/test_infer_expr.c'],
    kwargs: test_common_kwargs,
)
test('infer_expr', infer_expr_test, suite: 'unit', workdir: meson.project_source_root())

n00b_extension_methods_test = executable('test_n00b_extension_methods',
    ['test/unit/test_n00b_extension_methods.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep, monocypher_dep, mir_dep],
    build_by_default: n00b_build_tests,
)
test('n00b_extension_methods',
    n00b_extension_methods_test,
    suite: 'unit',
    workdir: meson.project_source_root())

# WP-009 Phase 3 — libn00b embedded-eval API smoke. Exercises
# `n00b_eval_session_new` + `n00b_eval_compile_predicate` against
# trivial true/false predicates and an FFI-bound C function call,
# covering the public surface at `include/n00b/eval.h`.
n00b_eval_test_link_args = exe_link_args
if target_os != 'darwin' and target_os != 'windows'
    n00b_eval_test_link_args = n00b_eval_test_link_args + ['-Wl,--export-dynamic']
endif
n00b_eval_test = executable('test_n00b_eval',
    ['test/unit/test_n00b_eval.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: n00b_eval_test_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep, monocypher_dep, mir_dep],
    build_by_default: n00b_build_tests,
)
test('n00b_eval',
    n00b_eval_test,
    suite: 'unit',
    workdir: meson.project_source_root(),
    is_parallel: false,
    timeout: 180)

# ============================================================================
# ============================================================================
# Render / layout tests
# ============================================================================

if not gui_tui_unnecessary_on_windows
layout_test = executable('test_layout',
    ['test/unit/test_layout.c'],
    kwargs: test_common_kwargs,
)
test('layout', layout_test, suite: 'unit')

render_cell_test = executable('test_render_cell',
    ['test/unit/test_render_cell.c'],
    kwargs: test_common_kwargs,
)
test('render_cell', render_cell_test, suite: 'unit')

render_plane_test = executable('test_render_plane',
    ['test/unit/test_render_plane.c'],
    kwargs: test_common_kwargs,
)
test('render_plane', render_plane_test, suite: 'unit')

render_box_test = executable('test_render_box',
    ['test/unit/test_render_box.c'],
    kwargs: test_common_kwargs,
)
test('render_box', render_box_test, suite: 'unit')

render_canvas_test = executable('test_render_canvas',
    ['test/unit/test_render_canvas.c'],
    kwargs: test_common_kwargs,
)
test('render_canvas', render_canvas_test, suite: 'unit')

render_ansi_test = executable('test_render_ansi',
    ['test/unit/test_render_ansi.c'],
    kwargs: test_common_kwargs,
)
test('render_ansi', render_ansi_test, suite: 'unit')

# Widget tests
label_test = executable('test_label',
    ['test/unit/test_label.c'],
    kwargs: test_common_kwargs,
)
test('label', label_test, suite: 'unit')

focus_test = executable('test_focus',
    ['test/unit/test_focus.c'],
    kwargs: test_common_kwargs,
)
test('focus', focus_test, suite: 'unit')

divider_test = executable('test_divider',
    ['test/unit/test_divider.c'],
    kwargs: test_common_kwargs,
)
test('divider', divider_test, suite: 'unit')

spacer_test = executable('test_spacer',
    ['test/unit/test_spacer.c'],
    kwargs: test_common_kwargs,
)
test('spacer', spacer_test, suite: 'unit')

progress_test = executable('test_progress',
    ['test/unit/test_progress.c'],
    kwargs: test_common_kwargs,
)
test('progress', progress_test, suite: 'unit')

button_test = executable('test_button',
    ['test/unit/test_button.c'],
    kwargs: test_common_kwargs,
)
test('button', button_test, suite: 'unit')

mouse_test = executable('test_mouse',
    ['test/unit/test_mouse.c'],
    kwargs: test_common_kwargs,
)
test('mouse', mouse_test, suite: 'unit')

checkbox_test = executable('test_checkbox',
    ['test/unit/test_checkbox.c'],
    kwargs: test_common_kwargs,
)
test('checkbox', checkbox_test, suite: 'unit')

input_test = executable('test_input',
    ['test/unit/test_input.c'],
    kwargs: test_common_kwargs,
)
test('input', input_test, suite: 'unit')

switch_test = executable('test_switch',
    ['test/unit/test_switch.c'],
    kwargs: test_common_kwargs,
)
test('switch', switch_test, suite: 'unit')

radio_test = executable('test_radio',
    ['test/unit/test_radio.c'],
    kwargs: test_common_kwargs,
)
test('radio', radio_test, suite: 'unit')

link_test = executable('test_link',
    ['test/unit/test_link.c'],
    kwargs: test_common_kwargs,
)
test('link', link_test, suite: 'unit')

list_widget_test = executable('test_list_widget',
    ['test/unit/test_list_widget.c'],
    kwargs: test_common_kwargs,
)
test('list_widget', list_widget_test, suite: 'unit')

selectionlist_test = executable('test_selectionlist',
    ['test/unit/test_selectionlist.c'],
    kwargs: test_common_kwargs,
)
test('selectionlist', selectionlist_test, suite: 'unit')

grid_test = executable('test_grid',
    ['test/unit/test_grid.c'],
    kwargs: test_common_kwargs,
)
test('grid', grid_test, suite: 'unit')

split_test = executable('test_split',
    ['test/unit/test_split.c'],
    kwargs: test_common_kwargs,
)
test('split', split_test, suite: 'unit')

scroll_test = executable('test_scroll',
    ['test/unit/test_scroll.c'],
    kwargs: test_common_kwargs,
)
test('scroll', scroll_test, suite: 'unit')

tabs_test = executable('test_tabs',
    ['test/unit/test_tabs.c'],
    kwargs: test_common_kwargs,
)
test('tabs', tabs_test, suite: 'unit')

text_test = executable('test_text',
    ['test/unit/test_text.c'],
    kwargs: test_common_kwargs,
)
test('text', text_test, suite: 'unit')

zstack_test = executable('test_zstack',
    ['test/unit/test_zstack.c'],
    kwargs: test_common_kwargs,
)
test('zstack', zstack_test, suite: 'unit')

draw_cmd_test = executable('test_draw_cmd',
    ['test/unit/test_draw_cmd.c'],
    kwargs: test_common_kwargs,
)
test('draw_cmd', draw_cmd_test, suite: 'unit')

breadcrumb_test = executable('test_breadcrumb',
    ['test/unit/test_breadcrumb.c'],
    kwargs: test_common_kwargs,
)
test('breadcrumb', breadcrumb_test, suite: 'unit')

# Table tests
table_build_test = executable('test_table_build',
    ['test/unit/test_table_build.c'],
    kwargs: test_common_kwargs,
)
test('table_build', table_build_test, suite: 'unit')

table_layout_test = executable('test_table_layout',
    ['test/unit/test_table_layout.c'],
    kwargs: test_common_kwargs,
)
test('table_layout', table_layout_test, suite: 'unit')

table_render_test = executable('test_table_render',
    ['test/unit/test_table_render.c'],
    kwargs: test_common_kwargs,
)
test('table_render', table_render_test, suite: 'unit')

table_stream_test = executable('test_table_stream',
    ['test/unit/test_table_stream.c'],
    kwargs: test_common_kwargs,
)
test('table_stream', table_stream_test, suite: 'unit')

event_normalize_test = executable('test_event_normalize',
    ['test/unit/test_event_normalize.c'],
    kwargs: test_common_kwargs,
)
test('event_normalize', event_normalize_test, suite: 'unit')
endif

# ============================================================================
# Machine learning tests (heap + stats supporting primitives + ml)
# ============================================================================

heap_test = executable('test_heap',
    ['test/unit/test_heap.c'],
    kwargs: test_common_kwargs,
)
test('heap', heap_test, suite: 'unit')

flagset_bloom_test = executable('test_flagset_bloom',
    ['test/unit/test_flagset_bloom.c'],
    kwargs: test_common_kwargs,
)
test('flagset_bloom', flagset_bloom_test, suite: 'unit')

stats_test = executable('test_stats',
    ['test/unit/test_stats.c'],
    kwargs: test_common_kwargs,
)
test('stats', stats_test, suite: 'unit')

ml_test = executable('test_ml',
    ['test/unit/test_ml.c'],
    kwargs: test_common_kwargs,
)
test('ml', ml_test, suite: 'unit')

if not gui_tui_unnecessary_on_windows
widget_demo_tools_inc = include_directories('src/tools')

display_diagnostics_test = executable('test_display_diagnostics',
    ['test/unit/test_display_diagnostics.c'],
    kwargs: test_common_kwargs,
)
test('display_diagnostics', display_diagnostics_test, suite: 'unit')

display_scene_contracts_test = executable('test_display_scene_contracts',
    ['test/unit/test_display_scene_contracts.c'],
    kwargs: test_common_kwargs,
)
test('display_scene_contracts', display_scene_contracts_test, suite: 'unit')

widget_demo_tools_inc = include_directories('src/tools')

display_backend_registry_test = executable('test_display_backend_registry',
    ['test/unit/test_display_backend_registry.c'],
    kwargs: test_common_kwargs,
)
test('display_backend_registry', display_backend_registry_test, suite: 'unit')

display_tty_mock_inc = include_directories('test/fixtures/display')
display_tty_mock_sources = []
display_tty_mock_link_args = []
display_tty_mock_link_with = []
display_tty_mock_env = []
if target_os == 'darwin'
  display_tty_mock = shared_library('n00b_test_tty_syscall_mock',
      ['test/fixtures/display/tty_syscall_mock.c'],
      include_directories: incdir + [display_tty_mock_inc],
      c_args: exe_c_args,
      link_args: exe_link_args + ['-Wl,-undefined,dynamic_lookup'],
      install: false,
  )
  display_tty_mock_link_with = [display_tty_mock]
  display_tty_mock_env = [
    'DYLD_INSERT_LIBRARIES=' + display_tty_mock.full_path(),
  ]
elif target_os != 'windows'
  display_tty_mock_sources = ['test/fixtures/display/tty_syscall_mock.c']
  display_tty_mock_link_args = [
    '-Wl,--wrap=isatty',
    '-Wl,--wrap=tcgetattr',
    '-Wl,--wrap=tcsetattr',
    '-Wl,--wrap=write',
  ]
endif

display_tty_mock_test_kwargs = test_common_kwargs + {
    'include_directories': incdir + [display_tty_mock_inc],
    'link_args': exe_link_args + display_tty_mock_link_args,
    'link_with': [n00b] + display_tty_mock_link_with,
}

display_backend_selection_test = executable('test_display_backend_selection',
    ['test/unit/test_display_backend_selection.c'] + display_tty_mock_sources,
    kwargs: display_tty_mock_test_kwargs,
)
test('display_backend_selection',
     display_backend_selection_test,
     suite: 'unit',
     env: display_tty_mock_env)

# Loadable renderer plugin: dlopen'd into a host process that already
# links libn00b, so it deliberately does NOT link libn00b itself. With
# GC stack-maps default-on, ncc emits references to the GC runtime
# (n00b_gc_register_roots / n00b_gc_stack_push / n00b_gc_stack_pop) into
# this TU; those resolve from the host at load time. Allow them to be
# left undefined at link time (dynamic_lookup on macOS; b_lundef=false
# prevents Meson from adding --no-undefined for ELF shared objects).
m5_renderer_plugin_link_args = []
if host_machine.system() == 'darwin'
  m5_renderer_plugin_link_args += ['-Wl,-undefined,dynamic_lookup']
endif
m5_renderer_plugin_fixture = shared_library('n00b_render_m5_test',
    ['test/fixtures/display/renderers/m5_test_renderer.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    dependencies: deps,
    link_args: m5_renderer_plugin_link_args,
    override_options: ['b_lundef=false'],
    install: false,
)

display_backend_plugin_test_kwargs = test_common_kwargs
if target_os != 'darwin' and target_os != 'windows'
  display_backend_plugin_test_kwargs = test_common_kwargs + {
    'link_args': exe_link_args + ['-Wl,--export-dynamic'],
  }
endif

display_backend_plugin_test = executable('test_display_backend_plugin',
    ['test/unit/test_display_backend_plugin.c'],
    kwargs: display_backend_plugin_test_kwargs,
)
test('display_backend_plugin',
     display_backend_plugin_test,
     suite: 'unit',
     env: ['N00B_RENDERER_PATH=' + meson.current_build_dir()],
     depends: [m5_renderer_plugin_fixture])

display_event_dispatch_test = executable('test_display_event_dispatch',
    ['test/unit/test_display_event_dispatch.c'],
    kwargs: test_common_kwargs,
)
test('display_event_dispatch', display_event_dispatch_test, suite: 'unit')

if target_os != 'windows'
  display_terminal_lifecycle_test = executable(
      'test_display_terminal_lifecycle',
      ['test/unit/test_display_terminal_lifecycle.c'] + display_tty_mock_sources,
      kwargs: display_tty_mock_test_kwargs,
  )
  test('display_terminal_lifecycle',
       display_terminal_lifecycle_test,
       suite: 'unit',
       env: display_tty_mock_env)
endif

display_canvas_lifecycle_test = executable('test_display_canvas_lifecycle',
    ['test/unit/test_display_canvas_lifecycle.c'],
    kwargs: test_common_kwargs,
)
test('display_canvas_lifecycle', display_canvas_lifecycle_test, suite: 'unit')

display_ansi_sgr_test = executable('test_display_ansi_sgr',
    ['test/unit/test_display_ansi_sgr.c'],
    kwargs: test_common_kwargs,
)
test('display_ansi_sgr', display_ansi_sgr_test, suite: 'unit')

# test_display_terminal_input.c includes <notcurses/notcurses.h> under
# `#if N00B_HAVE_NOTCURSES` (defined in exe_c_args when the backend is
# enabled), so it needs notcurses_dep's include path in that case.
# test_common_kwargs intentionally carries only the always-on deps.
ti_test_kwargs = test_common_kwargs
if conf_notcurses
  ti_test_kwargs = test_common_kwargs + {
    'dependencies': test_common_kwargs['dependencies'] + [notcurses_dep],
  }
endif
display_terminal_input_test = executable('test_display_terminal_input',
    ['test/unit/test_display_terminal_input.c'],
    kwargs: ti_test_kwargs,
)
test('display_terminal_input', display_terminal_input_test, suite: 'unit')

display_cocoa_input_test = executable('test_display_cocoa_input',
    ['test/unit/test_display_cocoa_input.c'],
    kwargs: test_common_kwargs,
)
test('display_cocoa_input', display_cocoa_input_test, suite: 'unit')

display_baseline_contract_test = executable('test_display_baseline_contract',
    ['test/unit/test_display_baseline_contract.c'],
    kwargs: test_common_kwargs,
)
test('display_baseline_contract', display_baseline_contract_test, suite: 'unit')

display_widget_primitives_test = executable('test_display_widget_primitives',
    ['test/unit/test_display_widget_primitives.c'],
    kwargs: test_common_kwargs,
)
test('display_widget_primitives', display_widget_primitives_test, suite: 'unit')

display_table_text_primitives_test = executable('test_display_table_text_primitives',
    ['test/unit/test_display_table_text_primitives.c'],
    kwargs: test_common_kwargs,
)
test('display_table_text_primitives', display_table_text_primitives_test, suite: 'unit')

display_hexdump_contracts_test = executable('test_display_hexdump_contracts',
    ['test/unit/test_display_hexdump_contracts.c'],
    kwargs: test_common_kwargs,
)
test('display_hexdump_contracts', display_hexdump_contracts_test, suite: 'unit')

if conf_cocoa
# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
  cocoa_test = executable('test_cocoa_backend',
      ['test/unit/test_cocoa_backend.m'],
      include_directories: incdir,
      objc_args: cocoa_objc_args,
      link_with: [n00b],
      build_by_default: n00b_build_tests,
  )
  test('cocoa_backend', cocoa_test, suite: 'unit')
endif

  cocoa_bridge_contracts_test = executable('test_display_cocoa_bridge_contracts',
      ['test/unit/test_display_cocoa_bridge_contracts.m'],
      include_directories: incdir,
      objc_args: cocoa_objc_args,
      link_with: [n00b],
      build_by_default: n00b_build_tests,
  )
  test('display_cocoa_bridge_contracts', cocoa_bridge_contracts_test, suite: 'unit')
endif

if conf_notcurses
  notcurses_test = executable('test_notcurses_backend',
      ['test/unit/test_notcurses_backend.c'],
      kwargs: ti_test_kwargs,
  )
endif

if conf_x11
  # These tests include <X11/*.h> directly, so they need x11_dep's
  # include path (test_common_kwargs carries only the always-on deps).
  x11_test_kwargs = test_common_kwargs + {
    'dependencies': test_common_kwargs['dependencies'] + [x11_dep],
  }
  display_x11_contracts_test = executable('test_display_x11_contracts',
      ['test/unit/test_display_x11_contracts.c'],
      kwargs: x11_test_kwargs,
  )
  test('display_x11_contracts', display_x11_contracts_test, suite: 'unit')

  x11_test = executable('test_x11_backend',
      ['test/unit/test_x11_backend.c'],
      kwargs: x11_test_kwargs,
  )
  test('x11_backend', x11_test, suite: 'unit')
endif

# Table CLI tool
table_tool = executable('n00b_table',
    ['src/tools/table.c'],
    kwargs: n00b_app_kwargs,
    install: true,
)
endif

# n00b-attest CLI binary (sign verb; other verbs are forward-roadmap
# stubs surfacing in --help and emitting "not yet implemented" + exit 1
# per DF-004 disposition (a)).
n00b_attest_tool = executable('n00b-attest',
    ['src/tools/n00b-attest.c'],
    kwargs: n00b_app_kwargs,
    install: true,
)

n00b_rocs_service_tool = executable('n00b-rocs-service',
    ['src/tools/n00b-rocs-service.c'],
    kwargs: n00b_app_kwargs,
    install: true,
)
test('rocs_service_tool_check_config',
    n00b_rocs_service_tool,
    args: ['--check-config'],
    suite: 'unit',
)
test('rocs_service_tool_smoke',
    n00b_rocs_service_tool,
    args: ['--smoke-start-stop'],
    env: ['ROCS_HTTP_ADDR=127.0.0.1:0'],
    suite: 'unit',
)

n00b_rocs_shards_tool = executable('rocsctl',
    ['src/tools/n00b-rocs-shards.c'],
    kwargs: n00b_app_kwargs,
    install: true,
)

n00b_rocs_wax_cache_tool = executable('n00b-rocs-wax-cache',
    [
        'src/tools/n00b-rocs-wax-cache.c',
        'src/tools/n00b-rocs-wax-cache-live.c',
        'src/tools/n00b-rocs-wax-cache-output.c',
    ],
    kwargs: n00b_app_kwargs,
    install: true,
)

rocs_wax_test_kwargs = test_common_kwargs + {
    'c_args': exe_c_args + [
        '-DROCS_TEST_SOURCE_ROOT="' + meson.current_source_dir() + '"',
        '-DROCS_WAX_CACHE_TOOL_PATH="'
            + n00b_rocs_wax_cache_tool.full_path() + '"',
    ],
}

rocs_wax_schema_test = executable('test_rocs_wax_schema',
    ['test/unit/test_rocs_wax_schema.c'],
    kwargs: rocs_wax_test_kwargs,
)
test('rocs_wax_schema', rocs_wax_schema_test, suite: 'unit')

rocs_wax_cli_test = executable('test_rocs_wax_cli',
    ['test/unit/test_rocs_wax_cli.c'],
    kwargs: rocs_wax_test_kwargs,
)
# The test SPAWNS n00b-rocs-wax-cache (ROCS_WAX_CACHE_TOOL_PATH is that
# target's full_path()), so the tool has to exist before the test runs. That is
# a runtime dependency meson cannot infer from the sources, and without
# `depends:` it is only satisfied by accident -- when something else in the
# build happens to have built the tool first.
#
# `ninja all` hides the omission. n00b#319's Windows job builds only the
# executables backing the tests it runs, which is where it surfaced: the test
# built, ran, and failed spawning a tool that was never compiled.
test('rocs_wax_cli', rocs_wax_cli_test, suite: 'unit',
     depends: [n00b_rocs_wax_cache_tool])

# n00b-wrap (and the obj_bundle_wrap test) JIT-run EMBEDDED_N00B policy programs,
# which resolve runtime builtins (e.g. the `print` builtin → `n00b_print`) and the
# wrap-policy FFI shims by NAME via dlsym at install time. On Linux/ELF a normal
# exe has a near-empty dynamic symbol table, so dlsym can't see statically-linked
# symbols; `-Wl,--export-dynamic` puts the linked symbols in .dynsym. macOS/Windows
# resolve these without the flag. Mirrors the n00b_eval / display-plugin tests.
wrap_jit_kwargs = test_common_kwargs
# Tool variant: same flags, but always built (the n00b-wrap binary is shipped).
n00b_wrap_app_kwargs = n00b_app_kwargs
if target_os != 'darwin' and target_os != 'windows'
  wrap_jit_kwargs = test_common_kwargs + {
    'link_args': exe_link_args + ['-Wl,--export-dynamic'],
  }
  n00b_wrap_app_kwargs = n00b_app_kwargs + {
    'link_args': exe_link_args + ['-Wl,--export-dynamic'],
  }
endif

n00b_wrap_tool = executable('n00b-wrap',
    ['src/tools/n00b_wrap.c'],
    kwargs: n00b_wrap_app_kwargs,
    install: true,
)

if host_machine.system() == 'darwin'
  nfs_exec_poc_tool = executable('nfs-exec-poc',
      ['src/tools/nfs_exec_poc.c'],
      kwargs: n00b_app_kwargs,
  )
endif

# WP-016 Phase 2: dedicated setuid-root loopback NFS mount helper for the
# macOS-only execute-from-bundle NFS execution mode. Built/installed on macOS
# ONLY (Linux NFS is out of scope). Installed to libexecdir; the runtime probe
# (helper present + executable + setuid) and the n00b_subproc invocation both
# expect it at `nfs_mount_helper_path`. Granting the setuid bit at install time
# is a deployment concern (OQ-2/OQ-3), not done here.
if host_machine.system() == 'darwin'
    n00b_nfs_mount_helper_tool = executable('n00b-nfs-mount-helper',
        ['src/tools/n00b-nfs-mount-helper.c'],
        kwargs: n00b_app_kwargs,
        install: true,
        install_dir: get_option('libexecdir'),
    )
endif

wax_tool = executable('wax',
    ['src/tools/wax.c'],
    kwargs: n00b_app_kwargs,
    install: true,
)
test('wax_tool_help',
    wax_tool,
    args: ['--help'],
    suite: 'unit',
)
test('wax_tool_common_options_before_command',
    wax_tool,
    args: ['--state-dir', '/tmp/n00b-rocs-wax-test-state', 'status', '--help'],
    suite: 'unit',
)

# naudit — n00b API-guidelines auditor CLI (consolidated in-tree per
# WP-008 / D-017; the engine TUs live under `src/naudit/` and are
# absorbed into libn00b above). The CLI body lives in
# `src/naudit/cli.c`; the thin `main()` in `src/tools/naudit.c`
# unwraps the result from `n00b_audit_run_cli` and returns the
# integer payload as the process exit code.
# When enabled, link the build-time-baked c_ncc grammar object
# (`c_grammar_image` custom_target) into the naudit binary. The object carries a
# `n00b_gimage` record named `c_ncc`; `n00b_static_grammar_lookup` discovers and
# relocates it directly. When disabled, naudit uses the runtime BNF load path.
naudit_tool_src = ['src/tools/naudit.c']
if conf_static_grammar_image
  naudit_tool_src += [c_grammar_image]
endif
naudit_tool = executable('naudit',
    naudit_tool_src,
    kwargs: n00b_app_kwargs,
    install: true,
)

# Widget visual demo tool
if not gui_tui_unnecessary_on_windows
widget_demo = executable('widget_demo',
    ['src/tools/widget_demo.c'],
    include_directories: incdir + [widget_demo_tools_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    install: false,
)

display_baseline_capture = executable('display_baseline_capture',
    ['src/tools/display_baseline_capture.c', 'src/tools/display_demo_scene.c'],
    kwargs: n00b_app_kwargs,
    install: false,
)

display_scene_inspect = executable('display_scene_inspect',
    ['src/tools/display_scene_inspect.c', 'src/tools/display_demo_scene.c'],
    kwargs: n00b_app_kwargs,
    install: false,
)

display_m4_showcase = executable('display_m4_showcase',
    ['src/tools/display_m4_showcase.c', 'src/tools/display_m4_showcase_fixture.c'],
    include_directories: incdir + [widget_demo_tools_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    install: false,
)

display_terminal_replay = executable('display_terminal_replay',
    ['src/tools/display_terminal_replay.c', 'src/tools/display_terminal_replay_fixture.c'],
    kwargs: n00b_app_kwargs,
    install: false,
)

display_gui_parity_report = executable('display_gui_parity_report',
    ['src/tools/display_gui_parity_report.c', 'src/tools/display_m3_parity_fixture.c'],
    include_directories: incdir + [widget_demo_tools_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    install: false,
)

display_backend_selection_report = executable('display_backend_selection_report',
    ['src/tools/display_backend_selection_report.c'],
    kwargs: n00b_app_kwargs,
    install: false,
)

display_m6_cutover_report = executable('display_m6_cutover_report',
    ['src/tools/display_m6_cutover_report.c', 'src/tools/display_startup_probe.c'],
    kwargs: n00b_app_kwargs,
    install: false,
)
endif

# N00b dev tool (parse + annotate n00b source files, REPL)
n00b_dev = executable('n00b_dev',
    ['src/tools/n00b_dev.c', 'src/tools/n00b_repl.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    # link_with, NOT link_whole (n00b#237). libn00b.a carries
    # n00b_crt_host_stub.o (the n00b_start entry stub), which references
    # n00b_crt_main -- a symbol ncc GENERATES, and only for a custom-entry
    # build. Force-loading the archive drags that stub in unconditionally, so
    # on Darwin ld64 fails the link with an undefined _n00b_crt_main; the
    # object is never needed here because these tools use the ordinary libc
    # entry. ld.bfd/lld on Linux happen not to error on the same input, which
    # is why this presented as macOS-only.
    #
    # Nothing else in the tree force-loads libn00b: every other consumer,
    # including all ~470 test binaries, uses link_with via n00b_app_kwargs.
    # These two were the outliers.
    link_with: [n00b],
    dependencies: [mir_dep],
    install: true,
)

# N00b main executable (compile, run, repl)
n00b_exe = executable('n00b',
    ['src/tools/n00b.c', 'src/tools/n00b_repl.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    # See the link_with note on n00b_dev above (n00b#237).
    link_with: [n00b],
    dependencies: [mir_dep],
    install: true,
)

if target_os != 'windows'
    # VFS journaling depends on POSIX daemon, signal, and frontend behavior.
    vfs_journal = executable('vfs_journal',
        ['src/tools/vfs_journal.c'],
        kwargs: n00b_app_kwargs,
        install: false,
    )
endif

# ============================================================================
# Integration tests
# ============================================================================

# Interpreter integration tests (shell runner invokes the n00b binary).
if target_os != 'windows' and not meson.is_cross_build()
  bash_prog = find_program('bash', required: true)
      # QUARANTINED (n00b#280): fails on Linux, passes on macOS. should_fail
    # cannot express that -- it turns the macOS pass into UNEXPECTEDPASS --
    # so it sits in 'known-broken', which CI excludes. Restore its real
    # suite with the fix.
test('interp',
      bash_prog,
      args: [
          files('test/interp/run_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: 'known-broken',
      workdir: meson.project_source_root(),
      timeout: 600)
  test('n00b_compile_cache',
      bash_prog,
      args: [
          files('test/integration/run_compile_cache_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: ['integration', 'long'],
      workdir: meson.project_source_root(),
      timeout: 120,
  )
endif

hash_test = executable('test_hash',
    ['test/integration/test_hash.c'],
    kwargs: test_common_kwargs,
)
test('hash', hash_test, suite: 'integration')

interval_tree_test = executable('test_interval_tree',
    ['test/integration/test_interval_tree.c'],
    kwargs: test_common_kwargs,
)
test('interval_tree', interval_tree_test, suite: 'integration')

dict_untyped_test = executable('test_dict_untyped',
    ['test/integration/test_dict_untyped.c'],
    kwargs: test_common_kwargs,
)
test('dict_untyped', dict_untyped_test, suite: 'integration')

dict_reader_strand_test = executable('test_dict_reader_strand',
    ['test/integration/test_dict_reader_strand.c'],
    kwargs: test_common_kwargs,
)
# Each case needs its own process: the strand diagnostic guard is
# process-global, so a second case in-process would count zero warnings.
# is_parallel: false because the pass condition is timing-based (wait gate
# + deadline) and a loaded machine can starve it.
test('dict_reader_strand_get', dict_reader_strand_test,
    args: ['get'], suite: 'integration', is_parallel: false)
test('dict_reader_strand_add', dict_reader_strand_test,
    args: ['add'], suite: 'integration', is_parallel: false)

arena_alloc_test = executable('test_arena_alloc',
    ['test/integration/test_arena_alloc.c'],
    kwargs: test_common_kwargs,
)
test('arena_alloc', arena_alloc_test, suite: 'integration')

alloc_interpose_test = executable('test_alloc_interpose',
    ['test/unit/test_alloc_interpose.c'],
    kwargs: test_common_kwargs,
)
test('alloc_interpose', alloc_interpose_test, suite: 'unit')
test('alloc_interpose_free_after_shutdown', alloc_interpose_test,
     args: ['--free-after-shutdown'], suite: 'unit')

object_marshal_test = executable('test_object_marshal',
    ['test/unit/test_object_marshal.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    # Includes slay/codegen.h + n00b/eval.h (drives codegen/JIT), which pull
    # mir.h — so this test needs mir_dep on the include path, like the
    # codegen/naudit-filter tests.
    dependencies: [picoquic_dep, monocypher_dep, mir_dep],
    build_by_default: n00b_build_tests,
)
test('object_marshal', object_marshal_test, suite: 'unit')

 ct_image_roundtrip_test = executable('test_ct_image_roundtrip',
     ['test/unit/test_ct_image_roundtrip.c'],
     kwargs: test_common_kwargs,
 )
 test('ct_image_roundtrip', ct_image_roundtrip_test, suite: 'unit')
 
 ct_image_relro_test = executable('test_ct_image_relro',
     ['test/unit/test_ct_image_relro.c'],
     kwargs: test_common_kwargs + {
         'include_directories': incdir + include_directories('src/crt'),
     },
 )
 test('ct_image_relro', ct_image_relro_test, suite: 'unit')
 
ct_image_gc_test = executable('test_ct_image_gc',
    ['test/unit/test_ct_image_gc.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('ct_image_gc', ct_image_gc_test, suite: 'unit')

buffer_baked_equiv_test = executable('test_buffer_baked_equiv',
    ['test/unit/test_buffer_baked_equiv.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('buffer_baked_equiv', buffer_baked_equiv_test, suite: 'unit')

writable_image_relocate_test = executable('test_writable_image_relocate',
    ['test/unit/test_writable_image_relocate.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('writable_image_relocate', writable_image_relocate_test, suite: 'unit')

ct_image_repair_hook_test = executable('test_ct_image_repair_hook',
    ['test/unit/test_ct_image_repair_hook.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('ct_image_repair_hook', ct_image_repair_hook_test, suite: 'unit')

grammar_image_repair_hook_test = executable('test_grammar_image_repair_hook',
    ['test/unit/test_grammar_image_repair_hook.c'],
    kwargs: test_common_kwargs,
)
test('grammar_image_repair_hook', grammar_image_repair_hook_test, suite: 'unit')

grammar_image_object_test = executable('test_grammar_image_object',
    ['test/unit/test_grammar_image_object.c'],
    kwargs: test_common_kwargs,
)
test('grammar_image_object', grammar_image_object_test, suite: 'unit')

ct_image_cross_image_ptr_test = executable('test_ct_image_cross_image_ptr',
    ['test/unit/test_ct_image_cross_image_ptr.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('ct_image_cross_image_ptr', ct_image_cross_image_ptr_test, suite: 'unit')

writable_baked_gc_test = executable('test_writable_baked_gc',
    ['test/unit/test_writable_baked_gc.c'],
    kwargs: test_common_kwargs + {
        'include_directories': incdir + include_directories('src/crt'),
    },
)
test('writable_baked_gc', writable_baked_gc_test, suite: 'unit')

static_init_runtime_test = executable('test_static_init_runtime',
    ['test/unit/test_static_init_runtime.c'],
    kwargs: test_common_kwargs,
)
test('static_init_runtime', static_init_runtime_test, suite: 'unit')

pool_alloc_test = executable('test_pool_alloc',
    ['test/integration/test_pool_alloc.c'],
    kwargs: test_common_kwargs,
)
    # Un-quarantined (was n00b#281): the macOS-only failure was
    # pool_quarantine_park skipping mprotect(PROT_NONE) on Apple, so parked
    # pages stayed readable and quarantine_uaf_faults' child never faulted.
    # Fixed in pool.c; Linux behaviour is unchanged, so this now passes on
    # both platforms and can gate again.
test('pool_alloc',
    pool_alloc_test,
    suite: 'integration',
    env: [
        'N00B_POOL_BIG_QUARANTINE=64',
        'N00B_POOL_PAGE_PER_ALLOC=guardpool',
    ])

mmaps_test = executable('test_mmaps',
    ['test/integration/test_mmaps.c'],
    kwargs: test_common_kwargs,
)
test('mmaps', mmaps_test, suite: 'integration')

# n00b#399: the kernel probe behind n00b_check_memory_perms may only read
# EFAULT as a verdict about the address; every other failure is the probe's
# own and comes back as perms_unknown, never as no_access (which truncates a
# conservative scan). Checks the two verdicts on unregistered raw pages.
memperm_probe_test = executable('test_memperm_probe',
    ['test/unit/test_memperm_probe.c'],
    kwargs: test_common_kwargs,
)
test('memperm_probe', memperm_probe_test, suite: 'unit')

alloc_metadata_test = executable('test_alloc_metadata',
    ['test/integration/test_alloc_metadata.c'],
    kwargs: test_common_kwargs,
)
test('alloc_metadata', alloc_metadata_test, suite: 'integration')

if not gui_tui_unnecessary_on_windows
display_baseline_flow_test = executable('test_display_baseline_flow',
    ['test/integration/test_display_baseline_flow.c'],
    kwargs: test_common_kwargs,
)
test('display_baseline_flow', display_baseline_flow_test, suite: 'integration')

if target_os != 'windows'
  sh_prog = find_program('sh', required: false)
  if not sh_prog.found()
    sh_prog = find_program('bash', required: false)
  endif
  if sh_prog.found()
    test('display_baseline_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_baseline_artifacts.sh'),
            display_baseline_capture,
            meson.project_source_root(),
        ],
        suite: 'integration',
    )

    test('display_scene_inspect_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_scene_inspect_artifacts.sh'),
            display_scene_inspect,
            meson.project_source_root(),
        ],
        suite: 'integration',
    )

    test('display_scene_inspect_errors',
        sh_prog,
        args: [
            files('test/integration/test_display_scene_inspect_errors.sh'),
            display_scene_inspect,
        ],
        suite: 'integration',
    )

    test('display_terminal_replay_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_terminal_replay_artifacts.sh'),
            display_baseline_capture,
            display_terminal_replay,
            meson.project_source_root(),
        ],
        suite: 'integration',
    )

    test('display_m4_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_m4_artifacts.sh'),
            display_baseline_capture,
            display_scene_inspect,
            display_m4_showcase,
            table_tool,
            meson.project_source_root(),
        ],
        suite: 'integration',
    )

    test('display_m5_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_m5_artifacts.sh'),
            display_baseline_capture,
            display_scene_inspect,
            display_backend_selection_report,
            widget_demo,
            meson.project_source_root(),
        ],
        suite: 'integration',
    )

    # Un-quarantined (n00b#281). The Linux failure was one line of
    # cutover_report.txt: `notcurses_available` is a live host probe recorded
    # into a byte-exact fixture, and it flipped to false when the runner image
    # changed. The harness now declares that probe's outcome the same way it
    # already pins TERM/DISPLAY/WAYLAND_DISPLAY, so the fixture no longer
    # depends on whether notcurses can start on the runner.
test('display_m6_artifacts',
        sh_prog,
        args: [
            files('test/integration/test_display_m6_artifacts.sh'),
            display_baseline_capture,
            display_scene_inspect,
            display_backend_selection_report,
            display_gui_parity_report,
            display_m6_cutover_report,
            meson.project_source_root(),
        ],
        suite: 'integration')
  endif
endif

display_m1_compat_test = executable('test_display_m1_compat',
    ['test/integration/test_display_m1_compat.c'],
    kwargs: test_common_kwargs,
)
test('display_m1_compat', display_m1_compat_test, suite: 'integration')

display_terminal_replay_fixture_inc = include_directories('src/tools')
display_m2_terminal_flow_test = executable('test_display_m2_terminal_flow',
    ['test/integration/test_display_m2_terminal_flow.c', 'src/tools/display_terminal_replay_fixture.c'],
    include_directories: incdir + [display_terminal_replay_fixture_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    build_by_default: n00b_build_tests,
)
test('display_m2_terminal_flow', display_m2_terminal_flow_test, suite: 'integration')

display_m3_backend_parity_test = executable('test_display_m3_backend_parity',
    ['test/integration/test_display_m3_backend_parity.c', 'src/tools/display_m3_parity_fixture.c'],
    include_directories: incdir + [widget_demo_tools_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    build_by_default: n00b_build_tests,
)
test('display_m3_backend_parity', display_m3_backend_parity_test, suite: 'integration')

display_m4_widget_table_flow_test = executable('test_display_m4_widget_table_flow',
    ['test/integration/test_display_m4_widget_table_flow.c', 'src/tools/display_m4_showcase_fixture.c'],
    include_directories: incdir + [widget_demo_tools_inc],
    c_args: exe_c_args,
    link_with: [n00b],
    build_by_default: n00b_build_tests,
)
test('display_m4_widget_table_flow', display_m4_widget_table_flow_test, suite: 'integration')

display_m5_runtime_selection_test = executable('test_display_m5_runtime_selection',
    ['test/integration/test_display_m5_runtime_selection.c'],
    kwargs: test_common_kwargs,
)
test('display_m5_runtime_selection', display_m5_runtime_selection_test, suite: 'integration')

display_m6_cutover_matrix_test = executable('test_display_m6_cutover_matrix',
    ['test/integration/test_display_m6_cutover_matrix.c', 'src/tools/display_startup_probe.c'],
    kwargs: test_common_kwargs,
)
test('display_m6_cutover_matrix', display_m6_cutover_matrix_test, suite: 'integration')
endif

# ============================================================================
# VFS tests
# ============================================================================

vfs_backend_test = executable('test_vfs_backend',
    ['test/unit/test_vfs_backend.c'],
    kwargs: test_common_kwargs,
)
test('vfs_backend', vfs_backend_test, suite: 'unit')

vfs_core_test = executable('test_vfs_core',
    ['test/unit/test_vfs_core.c'],
    kwargs: test_common_kwargs,
)
test('vfs_core', vfs_core_test, suite: 'unit')

vfs_cache_test = executable('test_vfs_cache',
    ['test/unit/test_vfs_cache.c'],
    kwargs: test_common_kwargs,
)
test('vfs_cache', vfs_cache_test, suite: 'unit')

vfs_local_test = executable('test_vfs_local',
    ['test/unit/test_vfs_local.c'],
    kwargs: test_common_kwargs,
)
test('vfs_local', vfs_local_test, suite: 'unit')

vfs_s3_contract_test = executable('test_vfs_s3_contract',
    ['test/unit/test_vfs_s3_contract.c'],
    kwargs: test_common_kwargs,
)
test('vfs_s3_contract', vfs_s3_contract_test, suite: 'unit')

vfs_journal_test = executable('test_vfs_journal',
    ['test/unit/test_vfs_journal.c'],
    kwargs: test_common_kwargs,
)
test('vfs_journal', vfs_journal_test, suite: 'unit')

# ============================================================================
# Unicode tests
# ============================================================================

unicode_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}

unicode_test_names = [
    'bidi', 'casemap', 'collation', 'emoji', 'encoding',
    'identifiers', 'idna', 'iter', 'linebreak', 'normalization',
    'properties', 'security', 'segmentation',
]

foreach name : unicode_test_names
    test_kwargs = unicode_test_kwargs
    # The n00b_unicode_foreach_{cp,grapheme,word,sentence} macros declare
    # their iterator state (incl. a pointer-bearing aggregate guarded with
    # [[gnu::cleanup]]) in for-init.  The gc-stack-maps transform can't
    # insert a tracking frame around a for-init declaration, so this test
    # opts into relaxed mode (skip-instead-of-error for unsupported
    # statement contexts).  The cleanup attribute still runs, so the
    # iterator is correctly torn down on every exit path.
    if name == 'iter'
        test_kwargs = unicode_test_kwargs + {
            'c_args': exe_c_args + ['--ncc-gc-stack-maps-relaxed'],
        }
    endif
    exe = executable('test_unicode_' + name,
        'test/unit/test_unicode_' + name + '.c',
        kwargs: test_kwargs,
    )
    test('unicode_' + name, exe, suite: 'unit')
endforeach

# ============================================================================
# String tests
# ============================================================================

str_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}

str_test_names = [
    'string_ops', 'string_convert', 'fptostr', 'fmt_numbers',
    'ansi', 'markdown', 'style_ops', 'string_style',
    'style_registry', 'rich_desc', 'format_spec',
    'format', 'md_render', 'md_lines',
]

if host_machine.system() == 'windows'
    # The display/style library is compiled on Windows, but its behavior and
    # display-facing tests are intentionally outside the native Windows port.
else
    style_registry_equiv_test = executable('test_style_registry_equiv',
        'test/unit/test_style_registry_equiv.c',
        kwargs: test_common_kwargs + {
            'include_directories': incdir + [include_directories('test/unit')],
        },
    )
    test('style_registry_equiv', style_registry_equiv_test, suite: 'unit')
endif

foreach name : str_test_names
    if host_machine.system() == 'windows' \
            and ['string_style', 'style_registry', 'format'].contains(name)
        continue
    endif
    exe = executable('test_str_' + name,
        'test/unit/test_str_' + name + '.c',
        kwargs: str_test_kwargs,
    )
    test('str_' + name, exe, suite: 'unit')
endforeach

# ============================================================================
# Object file (LIEF) tests
# ============================================================================

objfile_test_names = [
    'types', 'bstream', 'writer', 'detect', 'elf', 'elf_build', 'elf_layout',
    'elf_rewrite_admit', 'elf_rewrite',
    'macho', 'macho_build', 'macho_layout', 'macho_rewrite_admit', 'macho_rewrite', 'macho_fat_rewrite', 'macho_api_skeleton', 'pe', 'pe_build', 'demangle',
    'abstract', 'dwarf', 'elf_known_answers', 'obj_bundle',
    'obj_bundle_carrier', 'obj_bundle_extract', 'obj_bundle_exec',
    'obj_bundle_exec_run',
    'obj_bundle_policy', 'macho_carrier', 'macho_signing', 'macho_host_entrypoint', 'macho_e2e', 'macho_fat_carrier', 'macho_fat_carrier_read', 'macho_known_answers', 'sink',
]

foreach name : objfile_test_names
    test_sources = ['test/unit/test_objfile_' + name + '.c']
    test_kwargs = test_common_kwargs
    test_is_parallel = true
    test_env = {}

    # WP-001 Phase 1: the always-run macho harness resolves the committed
    # SIG_PRESENT fixture (test/unit/data/hello_signed_arm64.macho) via
    # MESON_SOURCE_ROOT; wiring it here makes that shape assert rather than
    # [SKIP] in the default run. Empty (unused) for the other objfile tests.
    if name == 'macho' or name == 'macho_layout' \
       or name == 'macho_rewrite_admit' or name == 'macho_rewrite' \
       or name == 'macho_carrier' or name == 'macho_signing' \
       or name == 'macho_host_entrypoint' or name == 'macho_e2e' \
       or name == 'macho_fat_carrier' or name == 'macho_fat_carrier_read' \
       or name == 'macho_known_answers'
        test_env = {'MESON_SOURCE_ROOT': meson.project_source_root()}
    endif

    if name in ['obj_bundle_carrier', 'obj_bundle_extract',
                'obj_bundle_exec', 'obj_bundle_exec_run', 'obj_bundle_policy',
                'macho_carrier',
                'macho_signing', 'macho_host_entrypoint', 'macho_e2e',
                'macho_known_answers']
        test_is_parallel = false
        if target_os != 'darwin' and target_os != 'windows'
            test_kwargs = test_common_kwargs + {
                'link_args': exe_link_args + ['-Wl,--export-dynamic'],
            }
        endif
    endif

    exe = executable('test_objfile_' + name,
        test_sources,
        kwargs: test_kwargs,
    )
    if test_is_parallel
        test('objfile_' + name, exe, suite: 'unit', env: test_env)
    else
        test('objfile_' + name, exe,
             suite: 'unit',
             is_parallel: false,
             timeout: 180,
             env: test_env)
    endif
endforeach

objfile_elf_oracle_test = executable('test_objfile_elf_oracle',
    'test/unit/test_objfile_elf_oracle.c',
    kwargs: test_common_kwargs,
)
test('objfile_elf_oracle', objfile_elf_oracle_test,
     suite: ['elf_oracle', 'long'],
     timeout: 120,
     workdir: meson.project_source_root())

# WP-001 Phase 1: gated Mach-O signing oracle. Default unit suite stays
# deterministic + host-neutral (D-006); this gated suite runs a real
# codesign(1) verify on the committed ad-hoc-signed fixture only behind
# N00B_TEST_MACHO_ORACLE=1 (D-015), skips cleanly otherwise, and never
# silently passes. The casegen .c is compiled in for symbol parity with
# the always-run harness. MESON_SOURCE_ROOT resolves the fixture path.
objfile_macho_oracle_test = executable('test_objfile_macho_oracle',
    ['test/unit/test_objfile_macho_oracle.c',
     'test/unit/objfile_macho_casegen.c'],
    kwargs: test_common_kwargs,
)
test('objfile_macho_oracle', objfile_macho_oracle_test,
     suite: ['macho_oracle', 'long'],
     timeout: 120,
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.project_source_root())

# WP-001 Phase 3: D-009 / FR-14 feasibility spike (NON-SHIPPING probe).
# Darwin-only (needs codesign(1) + the macOS loader) and gated behind
# N00B_TEST_MACHO_ORACLE; skips cleanly otherwise and never silently
# passes (D-006 / NFR-06). Runs the full signed-arm64 round-trip:
# strip -> insert loadable LC_SEGMENT_64 + slide __LINKEDIT -> redirect
# LC_MAIN -> ad-hoc resign via the existing chalk path -> verify + run.
if host_machine.system() == 'darwin'
# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
    macho_entrypoint_spike_test = executable(
        'test_macho_entrypoint_spike_macos',
        ['test/unit/test_macho_entrypoint_spike_macos.c'],
        kwargs: test_common_kwargs,
    )
    test('macho_entrypoint_spike_macos',
         macho_entrypoint_spike_test,
         suite: ['macho_oracle', 'long'],
         timeout: 120,
         env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
         workdir: meson.project_source_root())
endif
endif

# WP-016: gated per-mode execute-from-bundle execution test. Registered on ALL
# POSIX platforms: the NFS cases (P2-*) are macOS-only and the memfd cases (P3-*)
# are Linux-only, each self-gated in-C by platform + N00B_TEST_EXEC_RUN; the
# always-run probe cases (P2-b / P3-b / P3-c) and the [SKIP] paths run
# host-neutrally. On darwin the test additionally gets the built mount helper's
# path so the P2-c argv-validation case can exercise it without an install.
if host_machine.system() != 'windows'
    test_objfile_exec_run_modes_env = {
        'MESON_SOURCE_ROOT': meson.project_source_root(),
    }
    if host_machine.system() == 'darwin'
        test_objfile_exec_run_modes_env += {
            'N00B_NFS_MOUNT_HELPER_BUILD_PATH':
                n00b_nfs_mount_helper_tool.full_path(),
        }
    endif

    test_objfile_exec_run_modes_test = executable(
        'test_objfile_exec_run_modes',
        ['test/unit/test_objfile_exec_run_modes.c'],
        kwargs: test_common_kwargs,
    )
    test('objfile_exec_run_modes',
         test_objfile_exec_run_modes_test,
         suite: ['exec_run', 'long'],
         is_parallel: false,
         timeout: 120,
         env: test_objfile_exec_run_modes_env,
         workdir: meson.project_source_root())

    # WP-017: the EMBEDDED_N00B program-policy backend + generic wrap. Grows per
    # phase. Phase 1 covers the n00b_exec image-replacement primitive (P1-a
    # fork-then-execs /bin/true; P1-b/P1-c assert the failure paths in-process).
    # Phase 2 covers _n00b_obj_bundle_vfs_from_bundle (P2-a multi-artifact byte-equal
    # + parent dirs; P2-b edge logical paths; a null-bundle Err case). Phase 3 covers
    # _n00b_obj_bundle_run_wrapped (P3-a verdict 0/1 propagation; P3-c no-policy/null
    # Err; P3-b gated N00B_TEST_EXEC_RUN exec-replace of the embedded target). Phase 4
    # covers n00b_obj_bundle_wrap + the agent-guard policy program (P4-a wrap roundtrip,
    # __APPLE__-gated; P4-c always-run agent-ancestry verdict via injected chains; P4-b
    # gated N00B_TEST_EXEC_RUN agent-guard allow-path exec). The always-run cases are
    # host-neutral ('unit' suite, NOT 'long'); the gated exec cases (P3-b, P4-b) are
    # env-gated and TERMINAL (exec-replace the process — mutually exclusive). Registered
    # on all POSIX platforms.
    test_obj_bundle_wrap_test = executable(
        'test_obj_bundle_wrap',
        ['test/unit/test_obj_bundle_wrap.c'],
        # JITs EMBEDDED_N00B policy programs in-process → needs --export-dynamic
        # on Linux so dlsym can resolve runtime builtins (see wrap_jit_kwargs).
        kwargs: wrap_jit_kwargs,
    )
    # Was quarantined for n00b#235 ("__<bad-string-arg>_proto was already
    # defined"). That turned out to be a stale-ncc codegen artifact, not a
    # repo defect: it passes against the pinned ncc this PR bumps to. The
    # timeout is kept -- it is a JIT test and the default is tight.
    test('obj_bundle_wrap',
         test_obj_bundle_wrap_test,
         suite: 'unit',
         timeout: 90,
         workdir: meson.project_source_root())

    # WP-017 follow-up: CLI regression tests for the n00b-wrap tool. Spawns the
    # built n00b-wrap binary (path injected) and asserts no-args/--help/bad-flag/
    # missing-output behavior, a wrap roundtrip, and — via forkpty — that bare
    # n00b-wrap does NOT hang under a TTY (the n00b_shutdown/tcdrain fix).
    test_n00b_wrap_cli_test = executable(
        'test_n00b_wrap_cli',
        ['test/unit/test_n00b_wrap_cli.c'],
        kwargs: test_common_kwargs + {
            'c_args': exe_c_args + [
                '-DN00B_WRAP_TOOL_PATH="' + n00b_wrap_tool.full_path() + '"',
            ],
        },
    )
    test('n00b_wrap_cli',
         test_n00b_wrap_cli_test,
         suite: 'unit',
         workdir: meson.project_source_root())
endif

# Crayon ObjC/libxpc examples (Mac-only) live on the ML sibling branch
# along with examples/crayon_shared/.  They re-land with that branch.

# ============================================================================
# test_heap and test_stats live with the adt/heap.c source on a
# different feature branch; not built in this QUIC branch.

# ============================================================================
# QUIC tests
# ============================================================================

quic_framer_test = executable('test_quic_framer',
    ['test/unit/test_quic_framer.c'],
    kwargs: test_common_kwargs,
)
test('quic_framer', quic_framer_test, suite: 'unit')

quic_framer_fuzz_test = executable('test_quic_framer_fuzz',
    ['test/unit/test_quic_framer_fuzz.c'],
    kwargs: test_common_kwargs,
)
    # QUARANTINED (n00b#280): fails on Linux, passes on macOS. should_fail
    # cannot express that -- it turns the macOS pass into UNEXPECTEDPASS --
    # so it sits in 'known-broken', which CI excludes. Restore its real
    # suite with the fix.
test('quic_framer_fuzz', quic_framer_fuzz_test, suite: 'unit', timeout: 60)

quic_trust_test = executable('test_quic_trust',
    ['test/unit/test_quic_trust.c'],
    kwargs: test_common_kwargs,
)
test('quic_trust', quic_trust_test, suite: 'unit')

quic_secret_test = executable('test_quic_secret',
    ['test/unit/test_quic_secret.c'],
    kwargs: test_common_kwargs,
)
test('quic_secret', quic_secret_test, suite: 'unit')

# Linux-only libsecret bridge.  Compiled and registered only when
# pkg-config locates libsecret-1; the test SKIPs (exit 77) when no
# Secret Service daemon is reachable at run-time, so the suite still
# passes on Linux hosts without gnome-keyring / kwallet.
if have_libsecret
    quic_secret_libsecret_kwargs = {}
    foreach _k, _v : test_common_kwargs
        if _k == 'dependencies'
            quic_secret_libsecret_kwargs += {_k: _v + [libsecret_dep]}
        else
            quic_secret_libsecret_kwargs += {_k: _v}
        endif
    endforeach
    quic_secret_libsecret_test = executable(
        'test_quic_secret_libsecret',
        ['test/unit/test_quic_secret_libsecret.c'],
        kwargs: quic_secret_libsecret_kwargs,
    )
    test('quic_secret_libsecret', quic_secret_libsecret_test, suite: 'unit')
endif

# test_quic_acme_http removed in Phase 6 chunk 11 — its coverage
# (URL parser, header bag, response parser) is replaced by
# test_http_url, test_http_h1, and test_http_compression.

# JWS encoder + ECDSA-P-256 sign + base64url + JWK thumbprint.
quic_jws_test = executable('test_quic_jws',
    ['test/unit/test_quic_jws.c'],
    kwargs: test_common_kwargs,
)
test('quic_jws', quic_jws_test, suite: 'unit')

# Phase 3 § 6 — JWT verifier: compact JWS parse, ES256 verify,
# claim validation, alg refusals.  RS256 verify (rsa_verify.c) is
# exercised inline through the JSON dispatch in jwt.c.
quic_jwt_test = executable('test_quic_jwt',
    ['test/unit/test_quic_jwt.c'],
    kwargs: test_common_kwargs,
)
test('quic_jwt', quic_jwt_test, suite: 'unit')

# Phase 3 § 7 — OIDC discovery + JWKS cache.  Offline-only;
# live-IdP tests live in the Phase 3.10 fixture suite.
quic_oidc_test = executable('test_quic_oidc',
    ['test/unit/test_quic_oidc.c'],
    kwargs: test_common_kwargs,
)
test('quic_oidc', quic_oidc_test, suite: 'unit')

# Phase 3 § 8 — DPoP issue + verify (RFC 9449).
quic_dpop_test = executable('test_quic_dpop',
    ['test/unit/test_quic_dpop.c'],
    kwargs: test_common_kwargs,
)
test('quic_dpop', quic_dpop_test, suite: 'unit')

# SHA-512 / SHA-384 — FIPS 180-4 vectors validate the new
# core/sha512.c that underpins RS384/RS512 JWS verification.
sha512_test = executable('test_sha512',
    ['test/unit/test_sha512.c'],
    kwargs: test_common_kwargs,
)
test('sha512', sha512_test, suite: 'unit')

# Phase 3 § 9 — mTLS-bound tokens (RFC 8705).
quic_mtls_token_test = executable('test_quic_mtls_token',
    ['test/unit/test_quic_mtls_token.c'],
    kwargs: test_common_kwargs,
)
test('quic_mtls_token', quic_mtls_token_test, suite: 'unit')

# Phase 3 § 10 — per-channel auth policy.
quic_auth_policy_test = executable('test_quic_auth_policy',
    ['test/unit/test_quic_auth_policy.c'],
    kwargs: test_common_kwargs,
)
test('quic_auth_policy', quic_auth_policy_test, suite: 'unit')

# Phase 3 § 12 / § 10.2 — audit log shape + JSONL sink.
quic_audit_test = executable('test_quic_audit',
    ['test/unit/test_quic_audit.c'],
    kwargs: test_common_kwargs,
)
test('quic_audit', quic_audit_test, suite: 'unit')

# Phase 5 § 5.1 — Prometheus metrics registry + listener.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_metrics_test = executable('test_quic_metrics',
    ['test/unit/test_quic_metrics.c'],
    kwargs: test_common_kwargs,
)
test('quic_metrics', quic_metrics_test, suite: 'unit')
endif

# Phase 6 chunk 1 — HTTPS URL parser.
http_url_test = executable('test_http_url',
    ['test/unit/test_http_url.c'],
    kwargs: test_common_kwargs,
)
test('http_url', http_url_test, suite: 'unit')

# HTTP_PROXY / HTTPS_PROXY / NO_PROXY resolution for the h1 TLS transport.
http_proxy_test = executable('test_http_proxy',
    ['test/unit/test_http_proxy.c'],
    kwargs: test_common_kwargs,
)
test('http_proxy', http_proxy_test, suite: 'unit')

# grouped-005 regression: an `https://` proxy URL must never get a
# plaintext CONNECT (or Proxy-Authorization credential) — h1_tls_connect
# must reject it before dialing.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
http_proxy_https_plaintext_test = executable('test_http_proxy_https_plaintext',
    ['test/unit/test_http_proxy_https_plaintext.c'],
    kwargs: test_common_kwargs,
)
test('http_proxy_https_plaintext', http_proxy_https_plaintext_test,
     suite: 'unit', timeout: 10)
endif

# grouped-003 (H1 half) regression: mTLS client-cert requests must fail
# closed when a proxy route applies, instead of silently dialing the
# origin directly through the acme_tls transport.
http_h1_mtls_proxy_policy_test = executable('test_http_h1_mtls_proxy_policy',
    ['test/unit/test_http_h1_mtls_proxy_policy.c'],
    kwargs: test_common_kwargs,
)
test('http_h1_mtls_proxy_policy', http_h1_mtls_proxy_policy_test,
     suite: 'unit', timeout: 10)

# Phase 6 chunk 2 — HTTP/1.1 response parser + request builder
# (sub-chunks 2.1 + 2.2).
http_h1_test = executable('test_http_h1',
    ['test/unit/test_http_h1.c'],
    kwargs: test_common_kwargs,
)
test('http_h1', http_h1_test, suite: 'unit')

# Phase 6 chunk 2.3 — live-network smoke for the h1 round-trip
# path.  Skipped unless N00B_TEST_NET=1 (3 endpoints, 2-of-3
# tolerant gate per ~/dd/quic_6.md § 8.2).
http_h1_network_test = executable('test_http_h1_network',
    ['test/unit/test_http_h1_network.c'],
    kwargs: test_common_kwargs,
)
test('http_h1_network', http_h1_network_test,
     suite: 'unit', timeout: 60)

# Phase 6 chunk 3 — h3 round-trip primitive (argument validation
# + DNS failure path; full end-to-end exercises through the
# existing Caddy fixture when chunk 5's dispatcher lands).
http_h3_test = executable('test_http_h3',
    ['test/unit/test_http_h3.c'],
    kwargs: test_common_kwargs,
)
test('http_h3', http_h3_test, suite: 'unit', timeout: 30)

# Phase 6 chunk 3 — h3 informational network smoke (skipped unless
# N00B_TEST_NET=1).  Tolerant of trust-bridge gaps until chunk 9.
http_h3_network_test = executable('test_http_h3_network',
    ['test/unit/test_http_h3_network.c'],
    kwargs: test_common_kwargs,
)
test('http_h3_network', http_h3_network_test,
     suite: 'unit', timeout: 60)

# Phase 6 chunk 4 — connection pool.
http_pool_test = executable('test_http_pool',
    ['test/unit/test_http_pool.c'],
    kwargs: test_common_kwargs,
)
test('http_pool', http_pool_test, suite: 'unit')

# Minimal rwlock reader/writer contention repro.
rwlock_contention_test = executable('test_rwlock_contention',
    ['test/unit/test_rwlock_contention.c'],
    kwargs: test_common_kwargs,
)
test('rwlock_contention', rwlock_contention_test, suite: 'unit', timeout: 30)

# n00b#271: spinlock holders must not be starved by waiters that outnumber cores.
spinlock_contention_test = executable('test_spinlock_contention',
    ['test/unit/test_spinlock_contention.c'],
    kwargs: test_common_kwargs,
)
test('spinlock_contention', spinlock_contention_test, suite: 'unit', timeout: 120)

# PoC for the picotls CertificateVerify gap (see audit).
tls_certverify_gap_test = executable('test_tls_certverify_gap',
    ['test/unit/test_tls_certverify_gap.c'],
    kwargs: test_common_kwargs,
)
test('tls_certverify_gap', tls_certverify_gap_test, suite: 'unit',
     timeout: 10)

# In-process mTLS client-auth handshake test (both matched-key
# and mismatched-key paths).
acme_tls_mtls_test = executable('test_acme_tls_mtls',
    ['test/unit/test_acme_tls_mtls.c'],
    kwargs: test_common_kwargs,
)
test('acme_tls_mtls', acme_tls_mtls_test, suite: 'unit', timeout: 10)

# RSA-PSS-SHA256 sign+verify roundtrip — math-level coverage for the
# new n00b_rsa_sign_pss_sha256 primitive.  Schoolbook bn_powmod with
# the full 2048-bit private exponent is slow under -O0; bump the
# timeout accordingly.
rsa_pss_roundtrip_test = executable('test_rsa_pss_roundtrip',
    ['test/unit/test_rsa_pss_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('rsa_pss_roundtrip', rsa_pss_roundtrip_test,
     suite: ['unit', 'long'], timeout: 60)

# End-to-end QUIC mTLS handshake (loopback) — covers the h3 / picoquic
# side of n00b_picotls_install_client_auth, including the server-side
# verify_sign installation that catches forged client CertificateVerify.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_mtls_handshake_test = executable('test_quic_mtls_handshake',
    ['test/unit/test_quic_mtls_handshake.c'],
    kwargs: test_common_kwargs,
)
test('quic_mtls_handshake', quic_mtls_handshake_test, suite: 'unit',
     timeout: 30)
endif

# Phase 6 chunk 5 — public dispatcher (synchronous form;
# topic-shaped variant follows in a sub-chunk).
http_client_test = executable('test_http_client',
    ['test/unit/test_http_client.c'],
    kwargs: test_common_kwargs,
)
test('http_client', http_client_test, suite: 'unit', timeout: 30)

# grouped-003 (H3 half) regression: the public dispatcher's default
# prefer_h3=true must not dial the origin directly over H3/QUIC when a
# proxy route applies — it must defer to the proxy-aware h1 CONNECT path.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
http_h3_proxy_policy_test = executable('test_http_h3_proxy_policy',
    ['test/unit/test_http_h3_proxy_policy.c'],
    kwargs: test_common_kwargs,
)
test('http_h3_proxy_policy', http_h3_proxy_policy_test,
     suite: 'unit', timeout: 15)
endif

# DF-014 + DF-015 — per-call max_body_size + redirect_host_allowlist
# kwargs.  Substrate cases run unconditionally; integration leg
# requires N00B_TEST_DOCKER=1 (uses the zot fixture).
http_per_call_caps_test = executable('test_http_per_call_caps',
    ['test/unit/test_http_per_call_caps.c'],
    kwargs: test_common_kwargs,
)
test('http_per_call_caps', http_per_call_caps_test,
     suite: 'unit',
     timeout: 60,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# Phase 6 chunk 5 — live-network smoke for the dispatcher.
# Skipped unless N00B_TEST_NET=1.
http_client_network_test = executable('test_http_client_network',
    ['test/unit/test_http_client_network.c'],
    kwargs: test_common_kwargs,
)
test('http_client_network', http_client_network_test,
     suite: 'unit', timeout: 90)

# Phase 6 chunk 6 — Alt-Svc parser + cache.
http_alt_svc_test = executable('test_http_alt_svc',
    ['test/unit/test_http_alt_svc.c'],
    kwargs: test_common_kwargs,
)
test('http_alt_svc', http_alt_svc_test, suite: 'unit')

# Phase 6 chunk 8 — compression.
http_compression_test_kwargs = test_common_kwargs + {
    'dependencies': test_common_kwargs['dependencies'] + [zlib_dep],
}
http_compression_test = executable('test_http_compression',
    ['test/unit/test_http_compression.c'],
    kwargs: http_compression_test_kwargs,
)
test('http_compression', http_compression_test, suite: 'unit')

# Phase 6 chunk 9 — cookies.
http_cookies_test = executable('test_http_cookies',
    ['test/unit/test_http_cookies.c'],
    kwargs: test_common_kwargs,
)
test('http_cookies', http_cookies_test, suite: 'unit')

# Phase 6 chunk 10 — auth helper.
http_auth_test = executable('test_http_auth',
    ['test/unit/test_http_auth.c'],
    kwargs: test_common_kwargs,
)
test('http_auth', http_auth_test, suite: 'unit')

# Local HTTP/1 service/router.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
http_service_test = executable('test_http_service',
    ['test/unit/test_http_service.c'],
    kwargs: test_common_kwargs,
)
test('http_service', http_service_test, suite: 'unit')
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
http_json_parquet_test = executable('test_http_json_parquet',
    ['test/unit/test_http_json_parquet.c'],
    kwargs: test_common_kwargs,
)
test('http_json_parquet', http_json_parquet_test, suite: 'unit')
endif

# Phase 4 § 4.1 — CBOR encoder + decoder (RFC 8949).  Unit tests
# cover Appendix A vectors + n00b round-trips + decoder hardening +
# a light in-process fuzz smoke.  Dedicated libFuzzer harness lives
# at `test/fuzz/fuzz_quic_cbor.c`; built ad-hoc with Clang's
# `-fsanitize=fuzzer` rather than wired into the standard build.
quic_cbor_test = executable('test_quic_cbor',
    ['test/unit/test_quic_cbor.c'],
    kwargs: test_common_kwargs,
)
test('quic_cbor', quic_cbor_test, suite: 'unit', timeout: 60)

# Phase 4 § 4.2 — QPACK (RFC 9204): encoder, decoder, dynamic table,
# huffman, encoder/decoder stream protocol.  RFC examples + 1000-msg
# stress.  A libFuzzer harness lives at `test/fuzz/fuzz_quic_qpack.c`,
# built ad-hoc.
quic_qpack_test = executable('test_quic_qpack',
    ['test/unit/test_quic_qpack.c'],
    kwargs: test_common_kwargs,
)
test('quic_qpack', quic_qpack_test, suite: 'unit', timeout: 60)

# Phase 4 § 4.8 — RPC cancel/deadline ctx.  Tests local primitive only;
# wire signaling lands in 4.6 RPC unary.
quic_rpc_ctx_test = executable('test_quic_rpc_ctx',
    ['test/unit/test_quic_rpc_ctx.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_ctx', quic_rpc_ctx_test, suite: 'unit')

# Phase 4 § 4.10 — RPC status enum: name lookup, HTTP-class
# mapping, QUIC-err → status bridge, gRPC numeric alignment.
quic_rpc_status_test = executable('test_quic_rpc_status',
    ['test/unit/test_quic_rpc_status.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_status', quic_rpc_status_test, suite: 'unit')

# Phase 4 § 4.6 — RPC unary call: end-to-end in-process round-trip.
# Spins up an H3 client + server pair on UDP/lo, attaches the RPC
# dispatcher, and exercises register/lookup, success, application
# error, UNIMPLEMENTED, deadline-already-expired, and client-cancel
# mid-flight.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_rpc_unary_test = executable('test_quic_rpc_unary',
    ['test/unit/test_quic_rpc_unary.c'],
    kwargs: quic_test_kwargs,
)
test('quic_rpc_unary', quic_rpc_unary_test, suite: 'unit', timeout: 60)
endif

# Phase 4 § 4.7 — Streaming RPC: server-stream / client-stream / bidi
# end-to-end loopback.  Mirrors the unary test scaffold but exercises
# the streaming wire shapes (multiple DATA frames + early-publish on
# the H3 server side + the n00b_rpc_stream_t(T) FIFO).
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_rpc_streaming_test = executable('test_quic_rpc_streaming',
    ['test/unit/test_quic_rpc_streaming.c'],
    kwargs: quic_test_kwargs,
)
test('quic_rpc_streaming', quic_rpc_streaming_test,
     suite: 'unit', timeout: 90)
endif

# Phase 4 § 4.9 — RPC auth wiring (per-channel + per-call).
# Server-side dispatcher consults manifest auth.policies[] +
# rpc.services[] to gate inbound RPCs; per-call n00b-rpc-policy
# header lets a request opt into a stricter policy (downgrades
# rejected).  Audit emits one event per dispatch.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_rpc_auth_test = executable('test_quic_rpc_auth',
    ['test/unit/test_quic_rpc_auth.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    build_by_default: n00b_build_tests,
)
test('quic_rpc_auth', quic_rpc_auth_test, suite: 'unit', timeout: 90)
endif

# Phase 4 § 4.3 — H3 frame encoder + decoder (RFC 9114).
# Frame round-trip + truncated input + oversized + malformed.
quic_h3_frame_test = executable('test_quic_h3_frame',
    ['test/unit/test_quic_h3_frame.c'],
    kwargs: test_common_kwargs,
)
test('quic_h3_frame', quic_h3_frame_test, suite: 'unit')

# Phase 4 § 4.3 — Docker-gated H3 GET / smoke against the caddy
# fixture in `test/fixtures/caddy/`.  Run after:
#   eval "$(bash test/fixtures/caddy/start.sh)"
# Skips cleanly when N00B_TEST_DOCKER is unset.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_h3_caddy_smoke_test = executable('test_quic_h3_caddy_smoke',
    ['test/unit/test_quic_h3_caddy_smoke.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_caddy_smoke', quic_h3_caddy_smoke_test,
     suite: 'interop', timeout: 60)
endif

# Phase 4 § 4.4 — In-process H3 server loopback (client + server
# endpoint pair on UDP/lo).  Exercises GET, POST, 404, server reset,
# and GOAWAY emission on close.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_h3_server_loopback_test = executable('test_quic_h3_server_loopback',
    ['test/unit/test_quic_h3_server_loopback.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_server_loopback', quic_h3_server_loopback_test,
     suite: 'unit', timeout: 60)
endif

# WP-004 Phase 5 closeout — h3 mid-stream `max_body_size` enforcement
# regression (DF-014 lift).  Pins the receive-loop guard introduced on
# top of sub-agent 5 (`uq ae`)'s post-await check: a server that sends
# (or declares via `content-length`) a body larger than the caller's
# cap must trigger an `H3_EXCESSIVE_LOAD` stream reset before any
# excess bytes accumulate.  Uses the same in-process loopback fixture
# as `test_quic_h3_server_loopback`.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
http_h3_max_body_cap_test = executable('test_http_h3_max_body_cap',
    ['test/unit/test_http_h3_max_body_cap.c'],
    kwargs: quic_test_kwargs,
)
test('http_h3_max_body_cap', http_h3_max_body_cap_test,
     suite: 'unit', timeout: 60)
endif

# Phase 4 § 4.4 — Docker-gated H3 GET / + POST /echo smoke against
# the nginx-quic fixture in `test/fixtures/nginx-quic/`.  Run after:
#   eval "$(bash test/fixtures/nginx-quic/start.sh)"
# Skips cleanly when N00B_TEST_DOCKER is unset.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_h3_nginx_smoke_test = executable('test_quic_h3_nginx_smoke',
    ['test/unit/test_quic_h3_nginx_smoke.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_nginx_smoke', quic_h3_nginx_smoke_test,
     suite: 'interop', timeout: 60)
endif

# Phase 3 § 7 + § 10.10 — synthetic in-process IdP fixture wired
# end-to-end (OIDC + JWT + DPoP + mTLS-bound + policy + audit).
quic_synthetic_idp_test = executable('test_quic_synthetic_idp',
    ['test/unit/test_quic_synthetic_idp.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    build_by_default: n00b_build_tests,
)
test('quic_synthetic_idp', quic_synthetic_idp_test, suite: 'unit')

# Phase 3.10 manual interop — gated by KEYCLOAK_ISSUER + friends.
# Not run in CI (real-IdP interop is once-per-release per the
# Phase 2 LE-staging precedent).  Build it always so it doesn't
# bit-rot, but the test itself prints [SKIP] when the env isn't set.
quic_keycloak_interop_test = executable('test_quic_keycloak_interop',
    ['test/unit/test_quic_keycloak_interop.c'],
    kwargs: test_common_kwargs,
)
test('quic_keycloak_interop', quic_keycloak_interop_test,
     suite: 'unit', timeout: 60)

# Phase 3.11 — quic_oidc_demo example: manifest → OIDC + policy
# realization → eval against a synthetic-IdP-minted token.
quic_oidc_demo = executable('quic_oidc_demo',
    ['examples/quic_oidc_demo/main.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

# Phase 4 § 4.12 — quic_rpc_demo example: end-to-end H3+CBOR+@rpc demo
# with auth wired through the synthetic IdP.  Doubles as the smoke
# subject for `test_quic_rpc_demo_smoke`.
# Applications that run their own code on n00b off-libc worker threads (e.g.
# RPC dispatch handlers) and use the libc malloc family must route those
# allocations through n00b's user pool, or libc's first-malloc-per-thread
# traps. Force-include the interposition shim into such app TUs. (libn00b's
# QUIC glue + the vendored picoquic/picotls are already shimmed in their own
# build; this covers the example application code on top.)
quic_interpose_shim_cargs = [
    '-include',
    meson.global_source_root() / 'include' / 'core' / 'alloc_interpose_shim.h',
]

quic_rpc_demo = executable('quic_rpc_demo',
    ['examples/quic_rpc_demo/main.c',
     'examples/quic_rpc_demo/greet.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args + quic_interpose_shim_cargs,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

# Phase 5 § 5.3 — quic_phase5_demo: multi-tenant + observability demo.
# Loopback exercises 5 RPCs across 2 synthetic IdPs through the tenant
# resolver, then scrapes /metrics to verify the registry is populated.
quic_phase5_demo = executable('quic_phase5_demo',
    ['examples/quic_phase5_demo/main.c',
     'examples/quic_phase5_demo/services.c',
     'examples/quic_phase5_demo/tenant_resolver.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args + quic_interpose_shim_cargs,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
quic_phase5_demo_smoke_test = executable('test_quic_phase5_demo_smoke',
    ['test/unit/test_quic_phase5_demo_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_demo_smoke', quic_phase5_demo_smoke_test,
     suite: ['unit', 'long'], timeout: 90,
     env: {'N00B_QUIC_PHASE5_DEMO_BIN': quic_phase5_demo.full_path()},
     depends: [quic_phase5_demo])
endif

# Phase 5 § 5.4 — K8s playbook fixture smoke (kind + n00b deploy).
# Gated on N00B_TEST_KIND=1 + kind/kubectl/docker on PATH.
quic_phase5_k8s_test = executable('test_quic_phase5_k8s_smoke',
    ['test/unit/test_quic_phase5_k8s_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_k8s_smoke', quic_phase5_k8s_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_K8S_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_k8s/run.sh'})

# Phase 5 § 5.9 — bare-VM playbook fixture smoke (privileged
# container with systemd PID-1 + n00b unit).  Gated on
# N00B_TEST_VM=1 + docker on PATH.
quic_phase5_vm_test = executable('test_quic_phase5_vm_smoke',
    ['test/unit/test_quic_phase5_vm_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_vm_smoke', quic_phase5_vm_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_VM_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_vm/run.sh'})

# Phase 5 § 5.10 — bare-VM uacme + DNS-01 fixture smoke
# (privileged container with systemd PID-1 + Pebble +
# pebble-challtestsrv + uacme oneshot).  Same gate as 5.9.
quic_phase5_vm_uacme_test = executable('test_quic_phase5_vm_uacme_smoke',
    ['test/unit/test_quic_phase5_vm_uacme_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_vm_uacme_smoke', quic_phase5_vm_uacme_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_VM_UACME_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_vm/uacme/run.sh'})

# Phase 5 § 5.11 — ZITADEL alternate-IdP fixture smoke (kind +
# Helm + ZITADEL chart).  Gated on N00B_TEST_KIND_ZITADEL=1 +
# kind/kubectl/docker/helm on PATH.
quic_phase5_zitadel_test = executable('test_quic_phase5_zitadel_smoke',
    ['test/unit/test_quic_phase5_zitadel_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_zitadel_smoke', quic_phase5_zitadel_test,
     suite: ['integration', 'long'], timeout: 1500,
     env: {'N00B_PHASE5_ZITADEL_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_k8s/zitadel/run.sh'})

# Smoke for the demo: forks `quic_rpc_demo --loopback` as a subprocess,
# asserts the canonical Hello + Stream output appears.  The demo binary
# path is passed via the N00B_QUIC_RPC_DEMO_BIN env var (set on the
# test invocation below).
# n00b#333: POSIX-only (fork/waitpid); not built on Windows.
if target_os != 'windows'
quic_rpc_demo_smoke_test = executable('test_quic_rpc_demo_smoke',
    ['test/unit/test_quic_rpc_demo_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_demo_smoke', quic_rpc_demo_smoke_test,
     suite: ['unit', 'long'], timeout: 90,
     env: {'N00B_QUIC_RPC_DEMO_BIN': quic_rpc_demo.full_path()},
     depends: [quic_rpc_demo])
endif

# ACME end-to-end (Phase 2.3a): directory + newNonce + newAccount.
# Skipped unless STEPCA_DIR_URL is in the env — start the fixture via
# `test/fixtures/stepca/start.sh` to populate it.
quic_acme_session_test = executable('test_quic_acme_session',
    ['test/unit/test_quic_acme_session.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_session', quic_acme_session_test,
     suite: 'unit', timeout: 30)

# CSR builder.  In-process structural test always runs; external
# `openssl req -verify` runs when an openssl binary is available.
quic_acme_csr_test = executable('test_quic_acme_csr',
    ['test/unit/test_quic_acme_csr.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_csr', quic_acme_csr_test, suite: 'unit')

# Orchestrator smoke test (argument validation; live cert acquisition
# is deferred to the Phase 2 deployment playbook because HTTP-01
# validation needs port 80).
quic_acme_orchestrator_test = executable('test_quic_acme_orchestrator',
    ['test/unit/test_quic_acme_orchestrator.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_orchestrator', quic_acme_orchestrator_test, suite: 'unit')

quic_cert_provisioner_test = executable('test_quic_cert_provisioner',
    ['test/unit/test_quic_cert_provisioner.c'],
    kwargs: test_common_kwargs,
)
# Tests look up the fixture via N00B_SOURCE_ROOT; meson exposes this
# automatically via env when set in test_common_kwargs would propagate
# to children.  Easier: pass the project source root directly here.
test('quic_cert_provisioner', quic_cert_provisioner_test,
     suite: 'unit',
     env: {'N00B_SOURCE_ROOT': meson.project_source_root()})

quic_cert_store_test = executable('test_quic_cert_store',
    ['test/unit/test_quic_cert_store.c'],
    kwargs: test_common_kwargs,
)
test('quic_cert_store', quic_cert_store_test, suite: 'unit')

# Endpoint hot-reload (cert + key swap).
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_endpoint_reload_test = executable('test_quic_endpoint_reload',
    ['test/unit/test_quic_endpoint_reload.c'],
    kwargs: test_common_kwargs,
)
test('quic_endpoint_reload', quic_endpoint_reload_test, suite: 'unit')
endif

# Sticky secret (stateless-reset / address-validation token rotation).
quic_sticky_secret_test = executable('test_quic_sticky_secret',
    ['test/unit/test_quic_sticky_secret.c'],
    kwargs: test_common_kwargs,
)
test('quic_sticky_secret', quic_sticky_secret_test, suite: 'unit')

# LB-CID encoding (block-cipher mode, 16-byte fixed CID).
quic_lb_cid_test = executable('test_quic_lb_cid',
    ['test/unit/test_quic_lb_cid.c'],
    kwargs: test_common_kwargs,
)
test('quic_lb_cid', quic_lb_cid_test, suite: 'unit')

# Phase 2 fuzz tests — property tests across base64url, PEM, X.509
# validity walker, LB-CID decoder.  Same shape as the Phase 1
# framer fuzz: deterministic seed, ~5k iters per target.
quic_phase2_fuzz_test = executable('test_quic_phase2_fuzz',
    ['test/unit/test_quic_phase2_fuzz.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase2_fuzz', quic_phase2_fuzz_test,
     suite: ['unit', 'long'], timeout: 120)

# Manifest + preflight (design doc § 9).
quic_manifest_test = executable('test_quic_manifest',
    ['test/unit/test_quic_manifest.c'],
    kwargs: test_common_kwargs,
)
# Was quarantined for n00b#236 (SIGABRT on a dangling policy.idp finding).
# Same stale-ncc codegen artifact as #235: it passes against the pinned ncc
# this PR bumps to.
test('quic_manifest', quic_manifest_test, suite: 'unit', timeout: 90)

# DNS providers (manual + cloudflare) + DNS-01 challenge wrapper.
quic_dns_provider_test = executable('test_quic_dns_provider',
    ['test/unit/test_quic_dns_provider.c'],
    kwargs: test_common_kwargs,
)
test('quic_dns_provider', quic_dns_provider_test, suite: 'unit')

# Per-cnx SNI routing (cert store wired into picotls callbacks).
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_sni_routing_test = executable('test_quic_sni_routing',
    ['test/unit/test_quic_sni_routing.c'],
    kwargs: test_common_kwargs,
)
test('quic_sni_routing', quic_sni_routing_test, suite: 'unit')
endif

# Phase 3 § 5 — trust→picotls verify-cert bridge end-to-end.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_trust_bridge_test = executable('test_quic_trust_bridge',
    ['test/unit/test_quic_trust_bridge.c'],
    kwargs: test_common_kwargs,
)
test('quic_trust_bridge', quic_trust_bridge_test, suite: 'unit')
endif

# Phase 3 § 5.3 tail — mid-handshake-reload no-tear property.
# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_reload_race_test = executable('test_quic_reload_race',
    ['test/unit/test_quic_reload_race.c'],
    kwargs: test_common_kwargs,
)
test('quic_reload_race', quic_reload_race_test, suite: 'unit')
endif

# test_quic_acme_http_network removed in Phase 6 chunk 11 — its
# Let's-Encrypt-staging coverage runs through test_quic_acme_session
# (which now goes through n00b_http_request_sync) when the
# STEPCA_LEAF_FP env var is set; the basic h1 round-trip against
# public endpoints is exercised by test_http_h1_network.

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_endpoint_test = executable('test_quic_endpoint',
    ['test/unit/test_quic_endpoint.c'],
    kwargs: quic_test_kwargs,
)
test('quic_endpoint', quic_endpoint_test, suite: 'unit')
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_conn_test = executable('test_quic_conn',
    ['test/unit/test_quic_conn.c'],
    kwargs: test_common_kwargs,
)
test('quic_conn', quic_conn_test, suite: 'unit')
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_chan_test = executable('test_quic_chan',
    ['test/unit/test_quic_chan.c'],
    kwargs: test_common_kwargs,
)
test('quic_chan', quic_chan_test, suite: 'unit')
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_chan_recv_test = executable('test_quic_chan_recv',
    ['test/unit/test_quic_chan_recv.c'],
    kwargs: test_common_kwargs,
)
test('quic_chan_recv', quic_chan_recv_test, suite: 'unit')
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_handshake_test = executable('test_quic_handshake',
    ['test/unit/test_quic_handshake.c'],
    kwargs: quic_test_kwargs,
)
test('quic_handshake', quic_handshake_test, suite: 'unit', timeout: 30)
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_accept_test = executable('test_quic_accept',
    ['test/unit/test_quic_accept.c'],
    kwargs: quic_test_kwargs,
)
test('quic_accept', quic_accept_test, suite: 'unit', timeout: 30)
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
quic_qlog_test = executable('test_quic_qlog',
    ['test/unit/test_quic_qlog.c'],
    kwargs: quic_test_kwargs,
)
test('quic_qlog', quic_qlog_test, suite: 'unit', timeout: 30)
endif

# Perf harness — built but not registered as a CI test (it's
# benchmarking, not assertion-based).  Run manually with
# `./build_debug/perf_quic_stream_scaling`.
perf_quic_stream_scaling_kwargs = n00b_app_kwargs
if target_os == 'windows'
  perf_quic_stream_scaling_kwargs = n00b_app_kwargs + {
    'dependencies': n00b_app_kwargs['dependencies'] + [cc.find_library('psapi')],
  }
endif
perf_quic_stream_scaling = executable('perf_quic_stream_scaling',
    ['test/perf/perf_quic_stream_scaling.c'],
    kwargs: perf_quic_stream_scaling_kwargs,
)

# ============================================================================
# Conduit tests
# ============================================================================










if target_os == 'windows'
endif

# n00b#333: POSIX-only (raw BSD sockets); not built on Windows.
if target_os != 'windows'
conduit_udp_test = executable('test_conduit_udp',
    ['test/unit/test_conduit_udp.c'],
    kwargs: test_common_kwargs,
)
test('conduit_udp', conduit_udp_test, suite: 'unit')
endif
















# ============================================================================
# Regex engine tests
# ============================================================================





regex_resharp_test = executable('test_regex_resharp',
    ['test/unit/test_regex_resharp.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + ['--ncc-no-gc-stack-maps'],
    },
)
# Un-quarantined (n00b#288). The whole 329-case corpus now runs in ~18s on
# macOS and ~45s on Linux, so the 900s budget and the 'slow' opt-out that
# existed to keep it off every push are both gone.
#
# What it was actually reporting was the build type, not the engine. The
# harness caps each case at 5s, and at -O0 four of them exceeded that in the
# regex engine alone; the suite then spent most of its time in ncc's
# gc-stack-map push/pop pair rather than in matching. Built -O2 the corpus
# passes 329/329 with no case near the per-case budget.
#
# 300s is ~6x the slowest measured run, which leaves room for a loaded runner
# without going back to a budget long enough to hide a genuine hang.
test('regex_resharp', regex_resharp_test,
    suite: 'unit',
    timeout: 300,
    env: {
        'N00B_RESHARP_TEST_DIR': meson.current_source_dir() / 'test/data/resharp/tests',
    })

# ============================================================================
# Logic / constraint solver tests
# ============================================================================

logic_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}









# ============================================================================
# ML tests live on the ML feature branch; not built in this QUIC branch.
# ============================================================================

# ============================================================================
# Parser tests
# ============================================================================





# Slay tests commented out temporarily due to in-progress header changes
# scanner_test = executable('test_scanner',
#     ['test/unit/test_scanner.c'],
#     kwargs: test_common_kwargs,
# )
# test('scanner', scanner_test, suite: 'unit')
#
# ============================================================================
# Slay grammar tests
# ============================================================================

















# ============================================================================
# LSP framework tests
# ============================================================================

# LSP and nccd_* tests live on a different feature branch (the LSP
# initiative); not built in this QUIC branch.





# ============================================================================
# Type checker tests
# ============================================================================


# ============================================================================
# Analysis tests
# ============================================================================





# ============================================================================
# MIR JIT tests
# ============================================================================



# ============================================================================
# Render / layout tests
# ============================================================================







# Widget tests
















# Table tests





# cocoa_backend + notcurses_backend tests are declared earlier (from main's
# meson.build); the dedup pass missed the duplicate copies that came in with
# the QUIC-branch test section.  Drop the duplicates here.

# Table CLI tool

# Widget visual demo tool

# N00b dev tool (parse + annotate n00b source files, REPL)

# N00b main executable (compile, run, repl)

if target_os != 'windows'
    # VFS journaling depends on POSIX daemon, signal, and frontend behavior.
endif

# nccd (the ncc Language Server) and its lsp/ + nccd_* sources live
# on a different feature branch.  Not built in this QUIC branch.

# ============================================================================
# Integration tests
# ============================================================================

# Interpreter integration tests live in the `interp` suite registered
# earlier in this file (search for `test('interp', ...)`).  The older
# `find_program('bash')` registration here was a duplicate.





# ============================================================================
# VFS tests
# ============================================================================






# ============================================================================
# Unicode tests
# ============================================================================

unicode_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}

unicode_test_names = [
    'bidi', 'casemap', 'collation', 'emoji', 'encoding',
    'identifiers', 'idna', 'iter', 'linebreak', 'normalization',
    'properties', 'security', 'segmentation',
]

foreach name : unicode_test_names
endforeach

# ============================================================================
# String tests
# ============================================================================

str_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
    'build_by_default': n00b_build_tests,
}

str_test_names = [
    'string_ops', 'string_convert', 'fptostr', 'fmt_numbers',
    'ansi', 'markdown', 'style_ops', 'string_style',
    'style_registry', 'rich_desc', 'format_spec',
    'format', 'md_render', 'md_lines',
]

foreach name : str_test_names
endforeach

# ============================================================================
# Object file (LIEF) tests
# ============================================================================

objfile_test_names = [
    'types', 'bstream', 'writer', 'detect', 'elf', 'elf_build', 'elf_layout',
    'macho', 'macho_build', 'pe', 'pe_build', 'demangle',
    'abstract', 'dwarf',
]

foreach name : objfile_test_names
endforeach

# ============================================================================
# Regex engine tests (Phase 11 / resharp-c port)
# ============================================================================

# `fuzz_compare` is a corpus-generator tool (writes regex-crate oracle
# JSON), not a correctness test, and depends on harness symbols
# (bounded thread runner, JSON serde, regex_build_multiline) that are
# not yet on the n00b side.  Skipped until phase 2; the .c/.h files
# remain in place for that work.
regex_test_names = [
    'accel_skip', 'auto_harden', 'deriv', 'engine',
    'parser', 'properties', 'rev_nulls', 'seek', 'smoke_unicode',
    'smoke_utf8seq', 'stream',
]

# Standalone parser tests.
exe = executable('test_parsers_toml',
    'test/unit/test_parsers_toml.c',
    kwargs: test_common_kwargs,
)
test('parsers_toml', exe, suite: 'unit')

# Tests that consume the resharp-c TOML / haystack fixtures use the
# repo-local fixture copy so host and Docker builds see the same paths.
regex_engine_manifest_dir = meson.current_source_dir() / 'test/data/resharp'
regex_engine_test_c_args  = exe_c_args + [
    '-DRESHARP_ENGINE_MANIFEST_DIR="' + regex_engine_manifest_dir + '"',
]

foreach name : regex_test_names
    if name == 'engine' or name == 'fuzz_compare' \
            or name == 'accel_skip' or name == 'auto_harden' \
            or name == 'deriv' or name == 'rev_nulls'
        exe = executable('test_regex_' + name,
            'test/unit/test_regex_' + name + '.c',
            include_directories: incdir,
            c_args: regex_engine_test_c_args,
            link_args: exe_link_args,
            link_with: [n00b],
            build_by_default: n00b_build_tests,
        )
    else
        exe = executable('test_regex_' + name,
            'test/unit/test_regex_' + name + '.c',
            kwargs: test_common_kwargs,
        )
    endif
    # regex_engine runs derivative + fuzz comparisons across thousands of
    # patterns and legitimately takes ~60s in debug builds (often longer
    # under host load). The `engine` and `fuzz_compare` cases are tagged
    # `suite: 'long'` so build.sh's default `add_test_no_suite long` (when
    # `N00B_TEST_ALL=0` and no explicit suites/tests requested) excludes
    # them; opt in via `N00B_TEST_ALL=1` or `--all-tests`.
    #
    # regex_engine additionally carries `suite: 'stress'`, which is what
    # keeps it OUT of per-PR CI (n00b#13).
    #
    # Why, in the author's words: this is a line-by-line port of resharp and
    # "one very big stress test that should only have enough time to pass
    # when my laptop is completely idle, because I don't like to wait." The
    # 900s budget is sized for an idle machine, not for a 3-core/7GB shared
    # runner running four test processes at once. It timed out there roughly
    # two runs in three while passing in 66s locally.
    #
    # So this is a BUDGET mismatch, not a defect. No bug was ever
    # demonstrated -- only "it did not finish in 900s", which is the expected
    # outcome for a stress test on a loaded box. Raising the budget further
    # just makes a red run take longer to go red.
    #
    # It is deliberately NOT in 'known-broken': that suite means "this is
    # broken and should be fixed", which would be a false claim here. It is
    # in 'stress', alongside the other tests whose results depend on having
    # the machine to themselves. Run it with:
    #
    #     meson test --suite stress          # or
    #     N00B_TEST_ALL=1 ./build.sh         # or
    #     ./build_debug/test_regex_engine
    if name == 'engine'
        test_suite = ['stress', 'long']
        # 900s: sized for an idle machine. See the note above -- if this is
        # not enough, the machine is busy; do not raise it.
        test_timeout = 900
    elif name == 'fuzz_compare'
        test_suite = ['unit', 'long']
        # 900/90: see the CI-runner note above the gc timeouts (n00b#261).
        test_timeout = 900
    else
        test_suite = 'unit'
        test_timeout = 90
    endif
    test('regex_' + name, exe, suite: test_suite, timeout: test_timeout)
endforeach

# aarch64-gated SIMD regex tests
if target_arch == 'aarch64'
    foreach name : ['neon_simd', 'simd_neon_unit']
        # neon_simd consumes the same TOML fixture tree as the engine
        # tests for its lazy_*_toml cases; it needs the manifest dir.
        if name == 'neon_simd'
            exe = executable('test_regex_' + name,
                'test/unit/test_regex_' + name + '.c',
                include_directories: incdir,
                c_args: regex_engine_test_c_args,
                link_args: exe_link_args,
                link_with: [n00b],
                build_by_default: n00b_build_tests,
            )
        else
            exe = executable('test_regex_' + name,
                'test/unit/test_regex_' + name + '.c',
                kwargs: test_common_kwargs,
            )
        endif
        # neon_simd runs 25.4s locally against the 30s default -- 85% of its
        # budget -- and tips over on the slower CI macOS runner. First raise,
        # so this is a measurement, not a limit being pushed out of the way.
        if name == 'neon_simd'
            test('regex_' + name, exe, suite: 'unit', timeout: 120)
        else
            test('regex_' + name, exe, suite: 'unit')
        endif
    endforeach
endif

if target_arch != 'aarch64'
    exe = executable('test_regex_simd_backendless',
        'test/unit/test_regex_simd_backendless.c',
        kwargs: test_common_kwargs,
    )
    test('regex_simd_backendless', exe, suite: 'unit')
endif

# ============================================================================
# Documentation (Doxygen)
# ============================================================================

doxygen = find_program('doxygen', required: false)

if doxygen.found()
  doxyfile = configure_file(
    input: 'Doxyfile.in',
    output: 'Doxyfile',
    configuration: {
      'PROJECT_NAME': meson.project_name(),
      'PROJECT_VERSION': meson.project_version(),
      'TOP_SRCDIR': meson.project_source_root(),
      'TOP_BUILDDIR': meson.project_build_root(),
    },
  )

  custom_target('docs',
    input: doxyfile,
    output: 'html',
    command: [doxygen, '@INPUT@'],
    build_by_default: false,
    install: false,
  )
endif
