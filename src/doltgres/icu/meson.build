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

# The vendored ICU's data (design item 12), compiled in: icudata.pl writes the filtered archive (icudt79l.dat, made
# from the pinned ICU's data sources with filter.json) as a C source, and the library of it is linked by every program
# that links ICU, under the name ICU's own lookup of its data uses: icudt, the fork's suffix for ICU's names, ICU's
# major version, and _dat. The archive and its filter are here and not with the vendored sources, since a build may
# read no file of a subproject's folder. The root build includes this file from its ICU block, before the first
# program that links ICU.

doltgres_icudata_c = custom_target('icudata.c',
  input: ['icudata.pl', 'icudt79l.dat'],
  output: 'icudata.c',
  command: [perl, '@INPUT0@', '@INPUT1@', 'icudt_dg79_dat', '@OUTPUT@'],
)

doltgres_icudata_lib = static_library('doltgres_icudata',
  doltgres_icudata_c,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

doltgres_icudata = declare_dependency(link_with: doltgres_icudata_lib)
