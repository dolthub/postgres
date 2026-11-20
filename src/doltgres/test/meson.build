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

# The fork's tests that run against a server (design item 11).

# The compiler and flags the C port's compile-fail cases (t/117) are compiled with, the build's own, with the
# include folders Postgres' C finds its headers in; and its family, whose diagnostics a case's expected.tsv names.
doltgres_cc_family = cc.get_id() in ['msvc', 'gcc'] ? cc.get_id() : 'clang'
doltgres_cc = cc.cmd_array() + (doltgres_cc_family == 'msvc' ? ['/nologo'] : [])
foreach dir : postgres_inc_d
  doltgres_cc += [(doltgres_cc_family == 'msvc' ? '/I' : '-I') + (meson.project_build_root() / dir),
    (doltgres_cc_family == 'msvc' ? '/I' : '-I') + (meson.project_source_root() / dir)]
endforeach
doltgres_cc += cppflags + cflags + doltgres_cport_c_args

# pg's frontend test binary (the port text's section 7): std and pg's frontend binding built as frontend code with
# DOLTGRES_TEST_PG, the frontend test-only std::fs, and the frontend runner, which t/123 runs.
doltgres_cport_test_fe_pg = executable('doltgres_cport_test_fe_pg',
  doltgres_cport_std_sources + doltgres_cport_pg_frontend_sources + files('../std/c/test/fe_fs.c',
    'modules/doltgres_cport_test/doltgres_cport_test_fe_run.c', 'modules/doltgres_cport_test/doltgres_cport_test_judge.c',
    'modules/doltgres_cport_test/doltgres_cport_test_fe_pg.c'),
  c_args: doltgres_cport_c_args + doltgres_cport_frontend_args + ['-DDOLTGRES_TEST_PG'],
  include_directories: [doltgres_cport_pg_include, include_directories('modules/doltgres_cport_test')],
  dependencies: [frontend_code],
  kwargs: default_bin_args + {'install': false},
)

tests += {
  'name': 'doltgres',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
      'doltgres_pg_dir': meson.current_source_dir() / '..' / 'pg',
      'doltgres_cc': '
'.join(doltgres_cc),
      'doltgres_cc_family': doltgres_cc_family,
      'doltgres_cport_test_fe_pg': doltgres_cport_test_fe_pg.full_path(),
    },
    'tests': [
      't/001_binding.pl',
      't/002_preload.pl',
      't/003_shmem.pl',
      't/004_host.pl',
      't/005_file.pl',
      't/006_wal.pl',
      't/007_shared.pl',
      't/008_compression.pl',
      't/009_cache.pl',
      't/010_names.pl',
      't/011_contraction.pl',
      't/012_slots.pl',
      't/013_sweep.pl',
      't/014_long_locks.pl',
      't/015_pause.pl',
      't/016_exit_doors.pl',
      't/017_pages.pl',
      't/020_icu.pl',
      't/021_collator.pl',
      't/022_libc_locale.pl',
      't/026_status.pl',
      't/027_status_table.pl',
      't/023_kept_wal.pl',
      't/024_startup_open.pl',
      't/025_checkversioned.pl',
      't/040_registration.pl',
      't/041_database.pl',
      't/045_btree.pl',
      't/028_keys.pl',
      't/029_slot.pl',
      't/030_handler.pl',
      't/031_owned.pl',
      't/032_state.pl',
      't/033_scan.pl',
      't/034_compared.pl',
      't/035_tidrange.pl',
      't/036_parallel.pl',
      't/037_sizes.pl',
      't/038_insert.pl',
      't/039_runs.pl',
      't/044_removal.pl',
      't/042_snapshot.pl',
      't/043_serializable.pl',
      't/046_update.pl',
      't/050_fold.pl',
      't/052_vacuum.pl',
      't/054_insert_cost.pl',
      't/055_status_load.pl',
      't/061_unknown_index.pl',
      't/056_collect.pl',
      't/065_map_counts.pl',
      't/066_status_run.pl',
      't/047_rewrite.pl',
      't/053_spill.pl',
      't/062_fold_trigger.pl',
      't/057_identity.pl',
      't/062_size_sites.pl',
      't/064_value_frees.pl',
      't/067_layouts.pl',
      't/070_gen_binding.pl',
      't/101_cport_host.pl',
      't/102_cport_abort.pl',
      't/103_cport_encoding.pl',
    # The suites of the C port's modules that link simc, the Rust test crate, run only where those modules build.
    ] + (doltgres_rust ? [
      't/104_cport_std.pl',
      't/105_cport_chunks.pl',
      't/106_cport_simc.pl',
    ] : []) + [
      't/115_cport_encoding_pg19_orders.pl',
      't/110_cport_pg.pl',
      't/111_cport_shmem.pl',
      't/112_cport_pghost.pl',
    ] + (doltgres_rust ? [
      't/114_cport_trees.pl',
    ] : []) + [
      't/117_cport_compile_fail.pl',
    ] + (doltgres_rust ? [
      't/118_cport_repo.pl',
      't/113_cport_txn.pl',
      't/122_cport_doltgres.pl',
      't/119_cport_store.pl',
    ] : []) + [
      't/123_cport_fe_pg.pl',
      't/120_cport_access.pl',
    ],
    'deps': [doltgres_cport_test_fe_pg],
    # The injection points are cluster-wide, so disable installcheck
    'runningcheck': false,
  },
}

if get_option('injection_points')
  subdir('modules/doltgres_test')
endif

subdir('modules/doltgres_cport_test')
