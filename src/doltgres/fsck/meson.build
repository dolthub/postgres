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

# pg_checkversioned, the fork's check of a stopped cluster's versioned storage (design item D12): its C main, the
# C port's frontend library with the frontend wrappers gen_binding.pl generates, and Postgres' WAL reader, linked
# with Postgres' frontend code.

# pg_checkversioned links the C port's frontend configuration, whose fsck holds the check's entry (the C switch).
pg_checkversioned = executable('pg_checkversioned',
  files('pg_checkversioned.c', '../../backend/access/transam/xlogreader.c'),
  include_directories: [include_directories('.'), doltgres_cport_pg_include],
  dependencies: [frontend_code, lz4, zstd, doltgres_xxhash, doltgres_blake3],
  c_args: ['-DFRONTEND'],
  link_with: [doltgres_cport_frontend],
  kwargs: default_bin_args,
)
bin_targets += pg_checkversioned
