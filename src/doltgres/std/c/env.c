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
 * std's env::var (the port text's section 14): var_os, which is getenv in
 * every configuration, then OsString::into_string.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/env.h"
#include "doltgres/std/str.h"

/*
 * std_env_var is env::var of a key: the value as a String, or VarError's
 * NotPresent when the variable isn't set, or NotUnicode with its bytes when
 * they aren't UTF-8, into_string's map_err.
 */
bool
std_env_var(const char *key, StdString *result, StdVarError *error)
{
	const char *value = getenv(key);
	size_t		len;
	StdUtf8Error utf8;
	StdVec		bytes;

	if (value == NULL)
	{
		error->tag = STD_VAR_ERROR_NOT_PRESENT;
		return false;
	}
	len = strlen(value);
	std_vec_with_capacity_Unchecked(&bytes, len, 1, 1);
	std_vec_extend_from_slice_Unchecked(&bytes, value, len, 1, 1);
	if (!std_str_from_utf8(bytes.ptr, bytes.len, &utf8))
	{
		error->tag = STD_VAR_ERROR_NOT_UNICODE;
		error->u.not_unicode = bytes;
		return false;
	}
	*result = bytes;
	return true;
}

/* std_VarError_drop is VarError's drop: NotUnicode's OsString. */
void
std_VarError_drop(StdVarError *self)
{
	if (self->tag == STD_VAR_ERROR_NOT_UNICODE)
		std_vec_free_Checked(&self->u.not_unicode, 1, 1);
}
