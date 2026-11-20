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
 * The C form of std's env::var (the port text's section 14): the variable's
 * value through env::var_os, which is getenv in every configuration, as a
 * String when it is UTF-8.
 */
#ifndef DOLTGRES_STD_ENV_H
#define DOLTGRES_STD_ENV_H

#include "doltgres/std/vec.h"

/* StdVarErrorTag is VarError's variants. */
typedef enum StdVarErrorTag
{
	/* The variable isn't set. */
	STD_VAR_ERROR_NOT_PRESENT,

	/*
	 * The variable's value isn't UTF-8, which NotUnicode holds as an
	 * OsString.
	 */
	STD_VAR_ERROR_NOT_UNICODE,
} StdVarErrorTag;

/* StdVarError is env::VarError: why env::var gave no String, with NotUnicode's OsString's bytes. */
typedef struct StdVarError
{
	StdVarErrorTag tag;
	union
	{
		StdVec		not_unicode;
	}			u;
} StdVarError;

extern bool std_env_var(const char *key, StdString *result, StdVarError *error);
extern void std_VarError_drop(StdVarError *self);

#endif							/* DOLTGRES_STD_ENV_H */
