# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The fork's Rust workspace (design items 11 and 18), from which cargo.pl builds simc; the backend's postgres
# executable links the C port (doltgres_c and doltgres_cport).

# A tree holds the Rust workspace when it holds Cargo.toml; one without it, the C port's form, builds the C
# libraries, the server, and the C tests' modules.
doltgres_rust = fs.exists('Cargo.toml')
doltgres_c_sources = files('doltgres/disk_space.c', 'doltgres/in_progress.c', 'doltgres/kept_wal.c',
  'doltgres/locale_ctype.c')
if doltgres_rust
  doltgres_cargo = files('cargo.pl')

  # The folders Postgres' own C includes its headers from, each in the build tree and then the source tree as meson
  # searches them, and the vendored zstd's, which pg's build script reads Postgres' headers from.
  doltgres_includes = []
  foreach dir : postgres_inc_d
    doltgres_includes += [meson.project_build_root() / dir, meson.project_source_root() / dir]
  endforeach
  if zstd.found()
    doltgres_includes += subproject('zstd').get_variable('zstd_folder')
  endif

  # The environment of cargo.pl's builds: where Postgres' headers and the vendored zstd are, and the libclang bindgen
  # uses.
  doltgres_env = {
    'DOLTGRES_INCLUDES': (host_system == 'windows' ? ';' : ':').join(doltgres_includes),
    'DOLTGRES_LIBS': zstd.found() ? fs.parent(subproject('zstd').get_variable('zstd_lib').full_path()) : '',
    'LIBCLANG_PATH': get_option('libclang'),
  }
endif

# The C port's tokens' capability analysis (the port text's section 3), which the fork's clang builds check as
# errors; compilers without it take neither flag. A pointer of one type passed or assigned as another is an error in
# the fork's C under every compiler (MSVC's C4133), so that a shape's distinct types are refused as the Rust's are.
doltgres_cport_c_args = cc.get_supported_arguments('-Wthread-safety', '-Werror=thread-safety')
if cc.get_id() == 'msvc'
  doltgres_cport_c_args += '/we4133'
else
  doltgres_cport_c_args += '-Werror=incompatible-pointer-types'
endif

# The C port of the crates (the port text): std's forms and each ported crate's files, which the server links, as
# the C tests' modules link their closures. The server's library hides its symbols, so postgres exports none of them.
doltgres_cport_std_sources = files('std/c/alloc.c', 'std/c/any.c', 'std/c/btree.c', 'std/c/c_str.c', 'std/c/env.c', 'std/c/float.c', 'std/c/flt2dec.c', 'std/c/fmt.c', 'std/c/foldhash.c',
  'std/c/frozen_map.c', 'std/c/hash.c', 'std/c/hashmap.c', 'std/c/io.c', 'std/c/iter.c', 'std/c/path.c', 'std/c/ptr.c', 'std/c/rc.c', 'std/c/slice.c', 'std/c/str.c', 'std/c/sort.c', 'std/c/sync.c', 'std/c/time.c',
  'std/c/unicode.c', 'std/c/unwind.c', 'std/c/vec.c', 'std/c/vec_deque.c')
doltgres_cport_sources = doltgres_cport_std_sources
subdir('host/c')
subdir('chunks/c')
subdir('encoding/c')
subdir('pg/c')
subdir('access/c')
subdir('shmem/c')
subdir('pghost/c')
subdir('trees/c')
subdir('repo/c')
subdir('txn/c')
subdir('store/c')
subdir('doltgres/c')
subdir('fsck/fehost/c')
subdir('fsck/c')

# BLAKE3's C release (third_party/pins.tsv), which the C port's chunks hash addresses with.
doltgres_blake3 = subproject('blake3').get_variable('blake3_dep')

# xxHash's C release (third_party/pins.tsv), whose XXH3 the C port's chunks checksums with.
doltgres_xxhash = subproject('xxhash').get_variable('xxhash_dep')

# The C that gen_binding.pl generated: the wrappers that catch what a Postgres call raises, and the entry thunks
# that raise what an entry returns, compiled with the backend's flags into the postgres executable, with the fork's
# one file of Postgres' own C written again, the builtin provider's C.UTF-8 ctype methods (design item 12), the
# read of the kept WAL, whose page callback Postgres calls (design item 6), and the check of whether a transaction is
# in progress from any process, which reads the proc array under its lock (bugs row 89). Its symbols are hidden, and
# postgres exports only the entry thunks, which gen_binding.pl marks PGDLLEXPORT.
doltgres_c = static_library('doltgres_c',
  doltgres_c_sources + [doltgres_cport_pg_binding[0], doltgres_cport_pg_binding[1], doltgres_cport_pg_binding[2]],
  dependencies: [backend_code],
  gnu_symbol_visibility: 'hidden',
  kwargs: internal_lib_args,
)

doltgres_cport = static_library('doltgres_cport',
  doltgres_cport_sources + doltgres_cport_pg_export,
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_GLOBAL_ALLOCATOR'],
  include_directories: [doltgres_cport_pg_include, doltgres_cport_doltgres_include],
  dependencies: [doltgres_xxhash, backend_code, doltgres_blake3, zstd],
  gnu_symbol_visibility: 'hidden',
  kwargs: internal_lib_args + {'build_by_default': true},
)

# The C port's frontend configuration (the port text's section 14): the same files as frontend code, the
# configuration fsck's workspace builds the crates in, with std's frontend allocator and hooks; built and checked, and
# linked into nothing until fsck's C port, its user, lands.
# C11's atomics are std's atomics there (the port text's section 9); MSVC has them under /experimental:c11atomics,
# from Visual Studio 17.5.
doltgres_cport_frontend_args = ['-DFRONTEND']
if cc.get_id() == 'msvc'
  doltgres_cport_frontend_args += '/experimental:c11atomics'
endif
doltgres_cport_frontend = static_library('doltgres_cport_frontend',
  doltgres_cport_sources + doltgres_cport_pg_frontend_sources + doltgres_cport_fehost_sources +
    doltgres_cport_fsck_sources,
  c_args: doltgres_cport_c_args + doltgres_cport_frontend_args,
  include_directories: [doltgres_cport_pg_include, doltgres_cport_doltgres_include],
  dependencies: [doltgres_xxhash, frontend_code, doltgres_blake3, zstd],
  kwargs: internal_lib_args + {'build_by_default': true},
)

if doltgres_rust
  doltgres_host = run_command(perl, doltgres_cargo, 'host', meson.current_source_dir(), check: true).stdout().strip()
  # The system libraries Rust needs on this host that the compiler's driver doesn't already link (native-libs.tsv).
  doltgres_native_libs = []
  foreach line : fs.read('native-libs.tsv').split('\n')
    fields = line.split('\t')
    if fields.length() == 3 and fields[0] == doltgres_host
      doltgres_native_libs += fields[2].split()
    endif
  endforeach
  # simc, the C port's test crate (the port text's section 13): the simulation's host as the C port's host table and
  # sim's test API, which cargo.pl builds, in its own target folder, into the library the C tests' modules over sim
  # link, with the system libraries Rust needs.
  doltgres_simc = custom_target('simc',
    output: host_system == 'windows' ? 'simc.lib' : 'libsimc.o',
    depfile: 'libsimc.d',
    depend_files: files('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', 'simc/Cargo.toml', 'simc/build.rs',
      'simc/wrapper.h'),
    env: doltgres_env,
    command: [perl, doltgres_cargo, 'simc', meson.current_source_dir(),
      meson.current_build_dir() / 'target-simc', get_option('cassert') ? 'cassert' : 'release',
      '@OUTPUT0@', '@DEPFILE@'] + cc.cmd_array(),
  )
  doltgres_simc_link_args = [doltgres_simc.full_path()] + doltgres_native_libs
endif

# The check of each vendored library's names header against the symbols of the library built with it (names.pl,
# design item 15), built with everything else, so a build fails on a names.h that prefixes anything but the library's
# own external names. The symbols are listed by the compiler's own nm (gcc-nm or llvm-nm, which read objects built with
# link-time optimization) or else by nm, and configuring the build fails where none is found. On Windows a vendored
# library has no names header, and nothing is checked.
foreach vendored : [['zstd', zstd], ['lz4', lz4], ['blake3', doltgres_blake3]]
  if vendored[1].found() and subproject(vendored[0]).get_variable(vendored[0] + '_names').length() > 0
    custom_target(vendored[0] + '.names',
      input: [
        files('names.pl'),
        subproject(vendored[0]).get_variable(vendored[0] + '_names'),
        custom_target(vendored[0] + '.symbols',
          input: subproject(vendored[0]).get_variable(vendored[0] + '_lib'),
          output: vendored[0] + '.symbols',
          command: [find_program(cc.get_id() == 'gcc' ? 'gcc-nm' : 'llvm-nm', 'nm'), '-g', '@INPUT@'],
          capture: true,
        ),
      ],
      output: vendored[0] + '.names',
      command: [perl, '@INPUT0@', '@INPUT1@', '@INPUT2@', '@OUTPUT@'],
      build_by_default: true,
    )
  endif
endforeach

subdir('fsck')
