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
 * torn_tail_forged: a TornTail made outside file.c, with its parts, is
 * refused, as the Rust's private fields refuse a struct expression of
 * TornTail outside its module, since the C's ChunksFileTornTail names none
 * of its parts.
 */
#include "postgres.h"

#include "doltgres/chunks/file.h"

/* torn_tail_forged_case makes a TornTail of its own parts. */
extern void torn_tail_forged_case(void);

void
torn_tail_forged_case(void)
{
	ChunksFileTornTail tail;

	tail.end = 5;
}
