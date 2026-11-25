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
 * The edge test of std's Vec::extend_from_slice (the port text's section 3):
 * an empty slice at a null pointer, as the port's callers pass one, extends
 * by nothing through ptr::copy_nonoverlapping, which core lets copy no bytes
 * from a null pointer.
 */
#include "postgres.h"

#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* test_std_vec_tests_an_empty_slice_at_null_extends_by_nothing extends an empty vector by an empty slice at a null pointer. */
void
test_std_vec_tests_an_empty_slice_at_null_extends_by_nothing(void)
{
	StdVec		bytes = STD_VEC_NEW;

	std_vec_extend_from_slice_Checked(&bytes, NULL, 0, 1, 1);
	if (bytes.len != 0)
		std_panic(__FILE__, __LINE__, 0, "an empty slice extended a vector", sizeof("an empty slice extended a vector") - 1);
	std_vec_free_Checked(&bytes, 1, 1);
}
