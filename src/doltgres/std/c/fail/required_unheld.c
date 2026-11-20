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
 * required_unheld: a function that requires a capability, called without it,
 * is refused by the capability analysis, as the Rust refuses a call that
 * lacks the token its signature takes.
 */
#include "postgres.h"

#include "doltgres/std/capability.h"

/* required_unheld_token is the case's kind of capability. */
typedef struct required_unheld_token
{
	int			unused;
}			STD_CAPABILITY("token")
required_unheld_token;

/* required_unheld_held is the case's capability. */
extern required_unheld_token required_unheld_held;

/* required_unheld_use is a function its caller calls only holding the capability. */
extern void required_unheld_use(void) STD_REQUIRES(required_unheld_held);

/* required_unheld_case calls it holding nothing. */
extern void required_unheld_case(void);

void
required_unheld_case(void)
{
	required_unheld_use();
}
