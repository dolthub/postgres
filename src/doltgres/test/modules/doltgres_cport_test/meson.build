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

# The C port's test modules (the port text's section 13): one per test binary of the crates' ported tests, each built
# in that binary's configuration, in the test installation only.

# The modules' link: on ELF, -Bsymbolic binds a module's references to its own definitions, so the server's copy of
# the C port, which the postgres executable exports, doesn't take the place of the module's, built with its test
# switches; MSVC's and macOS's linkers bind them within the module already.
doltgres_cport_test_mod_args = pg_test_mod_args + {'link_args': cc.get_supported_link_arguments('-Wl,-Bsymbolic')}

# host's unit tests: host with DOLTGRES_TEST_HOST and no feature, as `cargo test -p host` builds it.
doltgres_cport_test_host = shared_module('doltgres_cport_test_host',
  doltgres_cport_std_sources + doltgres_cport_host_sources +
    files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_host.c'),
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_HOST'],
  kwargs: doltgres_cport_test_mod_args,
)
test_install_libs += doltgres_cport_test_host

# encoding's unit tests, encoding built with DOLTGRES_TEST_ENCODING over its closure (std, host, and chunks).
doltgres_cport_test_encoding = shared_module('doltgres_cport_test_encoding',
  doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
    doltgres_cport_encoding_sources + files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_encoding.c'),
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_ENCODING'],
  kwargs: doltgres_cport_test_mod_args + {
    'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3],
  },
)
test_install_libs += doltgres_cport_test_encoding

# encoding's ported pg19_orders integration test, encoding built without its test switch over its closure, with the
# folder of encoding's tests, whose recorded orders it reads.
doltgres_cport_encoding_tests = configuration_data()
doltgres_cport_encoding_tests.set_quoted('DOLTGRES_ENCODING_TESTS',
  meson.project_source_root() / 'src' / 'doltgres' / 'encoding' / 'tests')
doltgres_cport_encoding_tests_h = configure_file(output: 'doltgres_encoding_tests.h',
  configuration: doltgres_cport_encoding_tests)
doltgres_cport_test_encoding_pg19_orders = shared_module('doltgres_cport_test_encoding_pg19_orders',
  doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
    doltgres_cport_encoding_sources + files('../../../encoding/c/test/pg19_orders.c', 'doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c',
      'doltgres_cport_test_encoding_pg19_orders.c') + doltgres_cport_encoding_tests_h,
  include_directories: include_directories('.'),
  kwargs: doltgres_cport_test_mod_args + {
    'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3],
  },
)
test_install_libs += doltgres_cport_test_encoding_pg19_orders

if doltgres_rust
  # std's tests: core's own tests of the std forms that port a core algorithm (coretests' num/flt2dec and
  # alloctests' sort), with the runner's cases of a test's captured output, and with host (testing) and simc, whose
  # test-only std::fs the sort tests write their failure files through, and the zstd its sim links.
  doltgres_cport_test_std = shared_module('doltgres_cport_test_std',
    doltgres_cport_std_sources + doltgres_cport_host_sources +
      files('../../../std/c/test/lib.c', '../../../std/c/test/str.c', '../../../std/c/test/fmt/builders.c', '../../../std/c/test/num/mod.c',
      '../../../std/c/test/c_str.c', '../../../std/c/test/int128.c', '../../../std/c/test/io.c', '../../../std/c/test/time.c', '../../../std/c/test/sort/patterns.c',
      '../../../std/c/test/sort/zipf.c', '../../../std/c/test/sort/ffi_types.c', '../../../std/c/test/sort/tests.c',
      '../../../std/c/test/sort/partial.c',
      '../../../std/c/test/num/flt2dec/estimator.c', '../../../std/c/test/num/flt2dec/mod.c',
      '../../../std/c/test/num/flt2dec/random.c', '../../../std/c/test/num/flt2dec/strategy/dragon.c',
      '../../../std/c/test/num/flt2dec/strategy/grisu.c', '../../../std/c/test/vec.c', '../../../std/c/test/zero_size.c', 'doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_std.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': pg_test_mod_args['dependencies'] + [zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_std

  # The folder of chunks' test data, which its tests read their committed tables from.
  doltgres_cport_chunks_testdata = configuration_data()
  doltgres_cport_chunks_testdata.set_quoted('DOLTGRES_CHUNKS_TESTDATA',
    meson.project_source_root() / 'src' / 'doltgres' / 'chunks' / 'testdata')
  doltgres_cport_chunks_testdata_h = configure_file(output: 'doltgres_chunks_testdata.h',
    configuration: doltgres_cport_chunks_testdata)

  # chunks' unit tests, chunks built with DOLTGRES_TEST_CHUNKS over its closure (std and host), with simc, whose
  # simulation host its tests over sim run on, and the zstd simc's sim links, which a tree without the Rust
  # workspace can't build.
  doltgres_cport_test_chunks = shared_module('doltgres_cport_test_chunks',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_chunks.c') + doltgres_cport_chunks_testdata_h,
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_CHUNKS', '-DDOLTGRES_HOST_TESTING'],
    include_directories: include_directories('.'),
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_chunks

  # simc's tests, the C side of the Rust test crate's boundary, over std and host (testing), with simc and the zstd its
  # sim links.
  doltgres_cport_test_simc = shared_module('doltgres_cport_test_simc',
    doltgres_cport_std_sources + doltgres_cport_host_sources +
      files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_simc.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': pg_test_mod_args['dependencies'] + [zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_simc
endif

# pg's unit tests, pg built with DOLTGRES_TEST_PG over its closure (std), as `cargo test -p pg` builds it.
doltgres_cport_test_pg = shared_module('doltgres_cport_test_pg',
  doltgres_cport_std_sources + doltgres_cport_pg_sources +
    files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_pg.c'),
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_PG', '-DDOLTGRES_GLOBAL_ALLOCATOR'],
  include_directories: [doltgres_cport_pg_include],
  kwargs: doltgres_cport_test_mod_args + {'dependencies': pg_test_mod_args['dependencies'] + [zstd]},
)
test_install_libs += doltgres_cport_test_pg

# shmem's unit tests, shmem built with DOLTGRES_TEST_SHMEM over its closure (std and pg).
doltgres_cport_test_shmem = shared_module('doltgres_cport_test_shmem',
  doltgres_cport_std_sources + doltgres_cport_pg_sources + doltgres_cport_shmem_sources +
    files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_shmem.c'),
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_SHMEM', '-DDOLTGRES_GLOBAL_ALLOCATOR'],
  include_directories: [doltgres_cport_pg_include],
  kwargs: doltgres_cport_test_mod_args + {'dependencies': pg_test_mod_args['dependencies'] + [zstd]},
)
test_install_libs += doltgres_cport_test_shmem

# pghost's unit tests, pghost built with DOLTGRES_TEST_PGHOST over its closure (std, host, pg, and shmem).
doltgres_cport_test_pghost = shared_module('doltgres_cport_test_pghost',
  doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_pg_sources +
    doltgres_cport_shmem_sources + doltgres_cport_pghost_sources +
    files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_pghost.c'),
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_PGHOST', '-DDOLTGRES_GLOBAL_ALLOCATOR'],
  include_directories: [doltgres_cport_pg_include],
  kwargs: doltgres_cport_test_mod_args + {'dependencies': pg_test_mod_args['dependencies'] + [zstd]},
)
test_install_libs += doltgres_cport_test_pghost

# trees' unit tests, trees built with DOLTGRES_TEST_TREES over its closure (std, host, and chunks), with simc, whose
# simulation host its tests over sim run on, and the zstd simc's sim links, which a tree without the Rust workspace
# can't build.
if doltgres_rust
  doltgres_cport_test_trees = shared_module('doltgres_cport_test_trees',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      doltgres_cport_trees_sources + files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_trees.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_TREES', '-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_trees
endif

# repo's unit tests, repo built with DOLTGRES_TEST_REPO over its closure (std, host, chunks, and trees), with simc,
# whose simulation host its tests over sim run on, and the zstd simc's sim links.
if doltgres_rust
  doltgres_cport_test_repo = shared_module('doltgres_cport_test_repo',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      doltgres_cport_trees_sources + doltgres_cport_repo_sources +
      files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_repo.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_REPO', '-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_repo
endif

# txn's unit tests, txn built with DOLTGRES_TEST_TXN over its closure (std, host, chunks, and trees), with simc, whose
# simulation host its tests over sim run on, and the zstd simc's sim links, which a tree without the Rust workspace
# can't build.
if doltgres_rust
  doltgres_cport_test_txn = shared_module('doltgres_cport_test_txn',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      doltgres_cport_trees_sources + doltgres_cport_repo_sources + doltgres_cport_txn_sources +
      files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_txn.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_TXN', '-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_txn
endif

# store's unit tests, store built with DOLTGRES_TEST_STORE over its closure (std, host, chunks, trees, repo, and txn),
# with simc, whose simulation host its tests over sim run on, and the zstd simc's sim links.
if doltgres_rust
  doltgres_cport_test_store = shared_module('doltgres_cport_test_store',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      doltgres_cport_trees_sources + doltgres_cport_repo_sources + doltgres_cport_txn_sources +
      doltgres_cport_store_sources + files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_store.c'),
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_STORE', '-DDOLTGRES_HOST_TESTING'],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_store
endif

# The doltgres crate's unit tests, doltgres built with DOLTGRES_TEST_DOLTGRES over its closure (std, host, chunks,
# trees, repo, txn, encoding, pg, pghost, shmem, store, and access, pg's allocator the global one, as in pg's module),
# with simc and the zstd simc's sim links, as txn's module.
if doltgres_rust
  doltgres_cport_test_doltgres = shared_module('doltgres_cport_test_doltgres',
    doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
      doltgres_cport_trees_sources + doltgres_cport_repo_sources + doltgres_cport_txn_sources +
      doltgres_cport_encoding_sources + doltgres_cport_pg_sources + doltgres_cport_pghost_sources +
      doltgres_cport_shmem_sources + doltgres_cport_store_sources + doltgres_cport_access_sources +
      doltgres_cport_doltgres_sources +
      files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_doltgres.c') + [doltgres_cport_doltgres_test_tables],
    c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_DOLTGRES', '-DDOLTGRES_HOST_TESTING', '-DDOLTGRES_GLOBAL_ALLOCATOR'],
    include_directories: [doltgres_cport_doltgres_include, doltgres_cport_pg_include, doltgres_cport_access_include],
    kwargs: doltgres_cport_test_mod_args + {
      'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
      'link_args': doltgres_cport_test_mod_args['link_args'] + doltgres_simc_link_args,
      'link_depends': pg_test_mod_args['link_depends'] + [doltgres_simc],
    },
  )
  test_install_libs += doltgres_cport_test_doltgres
endif

# access' unit tests, access built with DOLTGRES_TEST_ACCESS over its closure (std, host, chunks, encoding, and pg).
doltgres_cport_test_access = shared_module('doltgres_cport_test_access',
  doltgres_cport_std_sources + doltgres_cport_host_sources + doltgres_cport_chunks_sources +
    doltgres_cport_encoding_sources + doltgres_cport_pg_sources + doltgres_cport_access_sources +
    files('doltgres_cport_test_run.c', 'doltgres_cport_test_judge.c', 'doltgres_cport_test_access.c') +
    [doltgres_cport_access_test_tables],
  c_args: doltgres_cport_c_args + ['-DDOLTGRES_TEST_ACCESS', '-DDOLTGRES_GLOBAL_ALLOCATOR'],
  include_directories: [doltgres_cport_pg_include, doltgres_cport_access_include],
  kwargs: doltgres_cport_test_mod_args + {
    'dependencies': [doltgres_xxhash] + pg_test_mod_args['dependencies'] + [doltgres_blake3, zstd],
  },
)
test_install_libs += doltgres_cport_test_access

# The extension's script, which names the functions of the modules that link simc only where they build.
test_install_data += files('doltgres_cport_test.control')
test_install_data += configure_file(input: 'doltgres_cport_test--1.0.sql', output: 'doltgres_cport_test--1.0.sql',
  configuration: {'SIMC_FUNCTIONS': doltgres_rust ? fs.read('doltgres_cport_test_simc.sql') : ''})
