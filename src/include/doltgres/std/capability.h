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
 * Clang's capability analysis (the port text's section 3): the attributes a
 * token's C form carries, which -Wthread-safety checks in the fork's clang
 * builds as errors, so that acquiring, releasing, and requiring a capability
 * is checked where the Rust's tokens were. Compilers without the attributes
 * get none, and check nothing.
 */
#ifndef DOLTGRES_STD_CAPABILITY_H
#define DOLTGRES_STD_CAPABILITY_H

#ifdef __clang__
#define STD_CAPABILITY_ATTRIBUTE(x) __attribute__((x))
#else
#define STD_CAPABILITY_ATTRIBUTE(x)
#endif

/* STD_CAPABILITY marks a type whose values are capabilities of a kind, named in diagnostics. */
#define STD_CAPABILITY(kind) STD_CAPABILITY_ATTRIBUTE(capability(kind))

/* STD_SCOPED_CAPABILITY marks a type whose values hold a capability for as long as they live, a guard's. */
#define STD_SCOPED_CAPABILITY STD_CAPABILITY_ATTRIBUTE(scoped_lockable)

/* STD_GUARDED_BY marks data that only a holder of a capability reads or writes. */
#define STD_GUARDED_BY(x) STD_CAPABILITY_ATTRIBUTE(guarded_by(x))

/* STD_PT_GUARDED_BY marks a pointer whose target only a holder of a capability reads or writes. */
#define STD_PT_GUARDED_BY(x) STD_CAPABILITY_ATTRIBUTE(pt_guarded_by(x))

/* STD_ACQUIRED_BEFORE and STD_ACQUIRED_AFTER order a capability's acquire before or after others'. */
#define STD_ACQUIRED_BEFORE(...) STD_CAPABILITY_ATTRIBUTE(acquired_before(__VA_ARGS__))
#define STD_ACQUIRED_AFTER(...) STD_CAPABILITY_ATTRIBUTE(acquired_after(__VA_ARGS__))

/* STD_REQUIRES and STD_REQUIRES_SHARED mark a function its caller calls only holding capabilities. */
#define STD_REQUIRES(...) STD_CAPABILITY_ATTRIBUTE(requires_capability(__VA_ARGS__))
#define STD_REQUIRES_SHARED(...) STD_CAPABILITY_ATTRIBUTE(requires_shared_capability(__VA_ARGS__))

/* STD_ACQUIRE and STD_ACQUIRE_SHARED mark a function that returns holding capabilities. */
#define STD_ACQUIRE(...) STD_CAPABILITY_ATTRIBUTE(acquire_capability(__VA_ARGS__))
#define STD_ACQUIRE_SHARED(...) STD_CAPABILITY_ATTRIBUTE(acquire_shared_capability(__VA_ARGS__))

/* STD_TRY_ACQUIRE marks a function that holds capabilities when it returns a value. */
#define STD_TRY_ACQUIRE(...) STD_CAPABILITY_ATTRIBUTE(try_acquire_capability(__VA_ARGS__))

/* STD_RELEASE and STD_RELEASE_SHARED mark a function that releases capabilities its caller held. */
#define STD_RELEASE(...) STD_CAPABILITY_ATTRIBUTE(release_capability(__VA_ARGS__))
#define STD_RELEASE_SHARED(...) STD_CAPABILITY_ATTRIBUTE(release_shared_capability(__VA_ARGS__))

/* STD_EXCLUDES marks a function its caller calls only not holding capabilities. */
#define STD_EXCLUDES(...) STD_CAPABILITY_ATTRIBUTE(locks_excluded(__VA_ARGS__))

/* STD_RETURN_CAPABILITY marks a function that returns a capability its result names. */
#define STD_RETURN_CAPABILITY(x) STD_CAPABILITY_ATTRIBUTE(lock_returned(x))

/*
 * STD_NO_THREAD_SAFETY_ANALYSIS marks a function whose own body the analysis
 * skips: one that makes or ends a capability its annotations name.
 */
#define STD_NO_THREAD_SAFETY_ANALYSIS STD_CAPABILITY_ATTRIBUTE(no_thread_safety_analysis)

#endif							/* DOLTGRES_STD_CAPABILITY_H */
