# Copyright (c) 2026, PostgreSQL Global Development Group

test_dolt_chunk_sources = files(
  'test_dolt_chunk.c',
)

if host_system == 'windows'
  test_dolt_chunk_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_dolt_chunk',
    '--FILEDESC', 'test_dolt_chunk - exercise and benchmark the dolt chunk store',])
endif

test_dolt_chunk = shared_module('test_dolt_chunk',
  test_dolt_chunk_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dolt_chunk

test_install_data += files(
  'test_dolt_chunk.control',
  'test_dolt_chunk--1.0.sql',
)

tests += {
  'name': 'test_dolt_chunk',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ['test_dolt_chunk'],
  },
  'tap': {
    'tests': ['t/001_journal.pl'],
  },
}
