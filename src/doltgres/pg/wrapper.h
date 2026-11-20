/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The Postgres and libzstd headers bindgen reads for the binding's types and
 * constants (design item 17), and the declarations of the generated wrappers.
 */
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"

#include <zdict.h>
#include <zstd.h>

#include "calls.h"
