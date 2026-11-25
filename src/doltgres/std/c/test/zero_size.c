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
 * The edge tests of std's zero-size accesses (the port text's section 3):
 * ptr::copy, ptr::write_bytes, and <[u8]>::starts_with of no bytes at a null
 * pointer, as the port's callers pass an empty slice, which core allows and
 * the C library's calls don't.
 */
#include "postgres.h"

#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"

/* test_std_zero_size_tests_bytes_start_with_an_empty_needle_at_null checks that empty bytes at null start with an empty needle at null. */
void
test_std_zero_size_tests_bytes_start_with_an_empty_needle_at_null(void)
{
	if (!std_bytes_starts_with(NULL, 0, NULL, 0))
		std_panic(__FILE__, __LINE__, 0, "empty bytes don't start with an empty needle",
				  sizeof("empty bytes don't start with an empty needle") - 1);
}

/* test_std_zero_size_tests_ptr_copy_of_nothing_takes_null copies no elements between null pointers. */
void
test_std_zero_size_tests_ptr_copy_of_nothing_takes_null(void)
{
	std_ptr_copy(NULL, NULL, 0, 1, 1);
}

/* test_std_zero_size_tests_ptr_write_bytes_of_nothing_takes_null writes no elements at a null pointer. */
void
test_std_zero_size_tests_ptr_write_bytes_of_nothing_takes_null(void)
{
	std_ptr_write_bytes(NULL, 0, 0, 1, 1);
}
