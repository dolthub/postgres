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
 * std's CString forms (the port text's section 9): alloc's ffi/c_str.rs.
 */
#ifndef DOLTGRES_STD_C_STR_H
#define DOLTGRES_STD_C_STR_H

#include "doltgres/std/vec.h"

/* StdCString is CString, a Box<[u8]> of its bytes and their NUL: the pointer and the length. */
typedef struct StdCString
{
	uint8	   *ptr;
	size_t		len;
} StdCString;

/* StdNulError is NulError: the first interior NUL's position, and the bytes CString::new was given. */
typedef struct StdNulError
{
	size_t		pos;
	StdVec		bytes;
} StdNulError;

/* StdFromBytesUntilNulError is FromBytesUntilNulError: bytes with no NUL. */
typedef struct StdFromBytesUntilNulError
{
	char		unused;
} StdFromBytesUntilNulError;

extern bool std_CString_new_String(StdString *t, StdCString *result, StdNulError *error);
extern bool std_CString_new_bytes(const uint8 *bytes, size_t len, StdCString *result, StdNulError *error);
extern void std_CString_default(StdCString *result);
extern const char *std_CString_as_ptr(const StdCString *self);
extern void std_CString_drop(StdCString *self);
extern void std_NulError_drop(StdNulError *self);
extern bool std_NulError_Debug_fmt(const void *value, struct StdFormatter *f);
extern void std_CString_into_bytes_with_nul(StdCString *self, StdVec *result);
extern bool std_CStr_from_bytes_until_nul(const uint8 *bytes, size_t len, const char **result,
										  StdFromBytesUntilNulError *error);
extern bool std_FromBytesUntilNulError_Debug_fmt(const void *value, struct StdFormatter *f);
extern void std_CStr_to_owned(const char *self, StdCString *result);
extern bool std_CStr_Debug_fmt(const void *value, struct StdFormatter *f);

/* test_std_c_str_tests_a_cstrs_debug_escapes_as_rusts is CStr's Debug's test, which std's test module runs. */
extern void test_std_c_str_tests_a_cstrs_debug_escapes_as_rusts(void);

#endif							/* DOLTGRES_STD_C_STR_H */
