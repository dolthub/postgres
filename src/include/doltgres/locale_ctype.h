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
 * The builtin provider's C.UTF-8 ctype methods, which the libc site gives a
 * locale whose collate name is a named locale's and whose ctype name is
 * C.UTF-8 (design item 12).
 */
#ifndef DOLTGRES_LOCALE_CTYPE_H
#define DOLTGRES_LOCALE_CTYPE_H

#include "utils/pg_locale.h"

/* doltgres_ctype_methods_builtin is the builtin provider's C.UTF-8 methods. */
extern const struct ctype_methods doltgres_ctype_methods_builtin;

#endif							/* DOLTGRES_LOCALE_CTYPE_H */
