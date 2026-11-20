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
 * The template of encoding's key.rs generic that holds a type parameter by
 * value (the port text's section 2): range<T, F>'s instance at a bound's
 * value type T, whose bounds hold T by value. An instance defines its
 * parameters and includes this file; every parameter is undefined at the
 * end.
 *
 * ENCODING_KEY_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * ENCODING_KEY_DECLARE generates the instance's types and declarations, and
 * ENCODING_KEY_DEFINE its definitions, which its declarations precede.
 *
 * ENCODING_KEY_RANGE_TS spells T, ENCODING_KEY_RANGE_T is T,
 * ENCODING_KEY_RANGE_T_COPY is defined for a T that is Copy, whose bounds
 * are passed by value, and ENCODING_KEY_RANGE_T_DROP(place), when T's drop
 * does something, drops a bound's value there.
 */

/* ENCODING_KEY_CAT pastes two tokens after expanding them. */
#define ENCODING_KEY_CAT_(a, b) a##b
#define ENCODING_KEY_CAT(a, b) ENCODING_KEY_CAT_(a, b)

/* ENCODING_KEY_DEF is ENCODING_KEY_SCOPE's storage class for a definition: static, or none for extern. */
#define ENCODING_KEY_DEF_static static
#define ENCODING_KEY_DEF_extern
#define ENCODING_KEY_DEF ENCODING_KEY_CAT(ENCODING_KEY_DEF_, ENCODING_KEY_SCOPE)

/* ENCODING_KEY_RANGE_NAME is the instance's range function. */
#define ENCODING_KEY_RANGE_NAME ENCODING_KEY_CAT(encoding_key_range, ENCODING_KEY_RANGE_TS)

/* ENCODING_KEY_RANGE_BOUND is the instance's bound tuple, a value and whether it is inclusive. */
#define ENCODING_KEY_RANGE_BOUND ENCODING_KEY_CAT(ENCODING_KEY_RANGE_NAME, _lower_t)

/*
 * ENCODING_KEY_RANGE_ARG is a bound parameter's type, by value for a Copy T
 * and by pointer to the caller's storage otherwise, and
 * ENCODING_KEY_RANGE_AT the bound it holds.
 */
#ifdef ENCODING_KEY_RANGE_T_COPY
#define ENCODING_KEY_RANGE_ARG ENCODING_KEY_RANGE_BOUND
#define ENCODING_KEY_RANGE_AT(bound) (&(bound))
#else
#define ENCODING_KEY_RANGE_ARG ENCODING_KEY_RANGE_BOUND *
#define ENCODING_KEY_RANGE_AT(bound) (bound)
#endif

#ifdef ENCODING_KEY_DECLARE

/* The instance's bound, (T, bool). */
typedef struct ENCODING_KEY_RANGE_BOUND
{
	ENCODING_KEY_RANGE_T f0;
	bool		f1;
}			ENCODING_KEY_RANGE_BOUND;

ENCODING_KEY_SCOPE void ENCODING_KEY_RANGE_NAME(StdVec *out, bool has_lower, ENCODING_KEY_RANGE_ARG lower,
												bool has_upper, ENCODING_KEY_RANGE_ARG upper,
												void (*write) (void *, StdVec *, ENCODING_KEY_RANGE_T *),
												void *write_captures, void (*write_drop) (void *));

#endif							/* ENCODING_KEY_DECLARE */

#ifdef ENCODING_KEY_DEFINE

/*
 * The instance's range appends a range that is not empty: its lower bound
 * (an infinite one first, then by value, an inclusive one before an
 * exclusive one at the same value) and its upper bound (by value, an
 * exclusive one before an inclusive one at the same value, and an infinite
 * one last). A bound is its value and whether it is inclusive, or nothing
 * when it is infinite. The function writes a bound's value.
 */
ENCODING_KEY_DEF void
ENCODING_KEY_RANGE_NAME(StdVec *out, bool has_lower, ENCODING_KEY_RANGE_ARG lower, bool has_upper,
						ENCODING_KEY_RANGE_ARG upper, void (*write) (void *, StdVec *, ENCODING_KEY_RANGE_T *),
						void *write_captures, void (*write_drop) (void *))
{
#ifdef ENCODING_KEY_RANGE_T_DROP
	volatile bool lower_live = has_lower;
	volatile bool upper_live = has_upper;
#endif
	volatile bool write_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_u8_push_Checked(out, 0x02);
		if (!has_lower)
			std_vec_u8_push_Checked(out, 0x01);
		else
		{
			std_vec_u8_push_Checked(out, 0x02);
#ifdef ENCODING_KEY_RANGE_T_DROP
			lower_live = false;
#endif
			write(write_captures, out, &ENCODING_KEY_RANGE_AT(lower)->f0);
			std_vec_u8_push_Checked(out, ENCODING_KEY_RANGE_AT(lower)->f1 ? 0x01 : 0x02);
		}
		if (!has_upper)
			std_vec_u8_push_Checked(out, 0x03);
		else
		{
			std_vec_u8_push_Checked(out, 0x02);
#ifdef ENCODING_KEY_RANGE_T_DROP
			upper_live = false;
#endif
			write(write_captures, out, &ENCODING_KEY_RANGE_AT(upper)->f0);
			std_vec_u8_push_Checked(out, ENCODING_KEY_RANGE_AT(upper)->f1 ? 0x02 : 0x01);
		}
		write_live = false;
		if (write_drop != NULL)
			write_drop(write_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (write_live && write_drop != NULL)
			write_drop(write_captures);
#ifdef ENCODING_KEY_RANGE_T_DROP
		if (upper_live)
			ENCODING_KEY_RANGE_T_DROP(&ENCODING_KEY_RANGE_AT(upper)->f0);
		if (lower_live)
			ENCODING_KEY_RANGE_T_DROP(&ENCODING_KEY_RANGE_AT(lower)->f0);
#endif
		std_unwind_resume(&unwind);
	}
}

#endif							/* ENCODING_KEY_DEFINE */

#undef ENCODING_KEY_CAT_
#undef ENCODING_KEY_CAT
#undef ENCODING_KEY_DEF_static
#undef ENCODING_KEY_DEF_extern
#undef ENCODING_KEY_DEF
#undef ENCODING_KEY_RANGE_NAME
#undef ENCODING_KEY_RANGE_BOUND
#undef ENCODING_KEY_RANGE_ARG
#undef ENCODING_KEY_RANGE_AT
#undef ENCODING_KEY_RANGE_T_COPY
#undef ENCODING_KEY_SCOPE
#undef ENCODING_KEY_DECLARE
#undef ENCODING_KEY_DEFINE
#undef ENCODING_KEY_RANGE_TS
#undef ENCODING_KEY_RANGE_T
#undef ENCODING_KEY_RANGE_T_DROP
