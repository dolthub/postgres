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
 * The C port of coretests' fmt/builders.rs tests of DebugStruct, DebugTuple,
 * and DebugList (library/coretests/tests/fmt/builders.rs, Rust 1.99.1), each
 * Debug's text plain and alternate, as format! writes it.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* RUST_FILE is builders.rs, where each test's assert_eq! reports. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "fmt" STD_SEP "builders.rs"

/* CORETESTS_FMT_BUILDERS_TRUE is the tests' &true. */
static const bool CORETESTS_FMT_BUILDERS_TRUE = true;

/* CORETESTS_FMT_BUILDERS_WORLD is the tests' &"world". */
static const StdStr CORETESTS_FMT_BUILDERS_WORLD = {"world", 5};

/* CORETESTS_FMT_BUILDERS_TEN, CORETESTS_FMT_BUILDERS_TWENTY are format_args!("{}/{}", 10, 20)'s arguments. */
static const int32 CORETESTS_FMT_BUILDERS_TEN = 10;
static const int32 CORETESTS_FMT_BUILDERS_TWENTY = 20;

/* CORETESTS_FMT_BUILDERS_TEN_TWENTY_PIECES is format_args!("{}/{}", 10, 20)'s template. */
static const StdFmtPiece CORETESTS_FMT_BUILDERS_TEN_TWENTY_PIECES[] = {STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1)};

/* CORETESTS_FMT_BUILDERS_TEN_TWENTY_ARGS is format_args!("{}/{}", 10, 20)'s arguments as Display. */
static const StdFmtArg CORETESTS_FMT_BUILDERS_TEN_TWENTY_ARGS[] = {
	{&CORETESTS_FMT_BUILDERS_TEN, std_i32_Display_fmt}, {&CORETESTS_FMT_BUILDERS_TWENTY, std_i32_Display_fmt},
};

/* CORETESTS_FMT_BUILDERS_TEN_TWENTY is the tests' &format_args!("{}/{}", 10, 20). */
static const StdFmtArguments CORETESTS_FMT_BUILDERS_TEN_TWENTY = {
	CORETESTS_FMT_BUILDERS_TEN_TWENTY_PIECES, lengthof(CORETESTS_FMT_BUILDERS_TEN_TWENTY_PIECES),
	CORETESTS_FMT_BUILDERS_TEN_TWENTY_ARGS,
};

/* coretests_fmt_builders_Arguments_Display_fmt is Arguments' Display: write of its template to the formatter's buffer. */
static bool
coretests_fmt_builders_Arguments_Display_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write(f->ops, f->buf, value);
}

/* coretests_fmt_builders_Arguments_Debug_fmt is Arguments' Debug: its Display. */
static bool
coretests_fmt_builders_Arguments_Debug_fmt(const void *value, StdFormatter *f)
{
	return coretests_fmt_builders_Arguments_Display_fmt(value, f);
}

/*
 * coretests_fmt_builders_check is a test's assert_eq! of a literal and
 * format! of a value's Debug, plain or alternate, at the assertion's line.
 */
static void
coretests_fmt_builders_check(uint32 line, const char *expected, StdFmtFn debug, bool alternate)
{
	StdFmtPiece pieces[] = {{NULL, 0, 0, {.alternate = alternate, .fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}}};
	StdFmtArg	args[] = {{NULL, debug}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdStr		left = {expected, strlen(expected)};
	StdString	right;

	std_fmt_format_Checked(&right, &arguments);
	STD_ASSERT_EQ(line, 9, left.len == right.len && memcmp(left.ptr, right.ptr, right.len) == 0, &left,
				  std_str_Debug_fmt, &right, std_String_Debug_fmt);
	std_vec_free_Checked(&right, 1, 1);
}

/* coretests_fmt_builders_debug_struct_test_empty_Foo_Debug_fmt is test_empty's Foo's Debug: debug_struct("Foo").finish(). */
static bool
coretests_fmt_builders_debug_struct_test_empty_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	return std_DebugStruct_finish(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_empty is debug_struct::test_empty. */
void
test_coretests_fmt_builders_debug_struct_test_empty(void)
{
	coretests_fmt_builders_check(14, "Foo", coretests_fmt_builders_debug_struct_test_empty_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(15, "Foo", coretests_fmt_builders_debug_struct_test_empty_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_single_Foo_Debug_fmt is test_single's Foo's Debug: debug_struct("Foo").field("bar", &true).finish(). */
static bool
coretests_fmt_builders_debug_struct_test_single_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	std_DebugStruct_field(&builder, "bar", 3, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_single is debug_struct::test_single. */
void
test_coretests_fmt_builders_debug_struct_test_single(void)
{
	coretests_fmt_builders_check(28, "Foo { bar: true }", coretests_fmt_builders_debug_struct_test_single_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(29, "Foo {\n    bar: true,\n}", coretests_fmt_builders_debug_struct_test_single_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_multiple_Foo_Debug_fmt is test_multiple's Foo's Debug: debug_struct("Foo").field("bar", &true).field("baz", &format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_struct_test_multiple_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	std_DebugStruct_field(&builder, "bar", 3, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugStruct_field(&builder, "baz", 3, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugStruct_finish(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_multiple is debug_struct::test_multiple. */
void
test_coretests_fmt_builders_debug_struct_test_multiple(void)
{
	coretests_fmt_builders_check(50, "Foo { bar: true, baz: 10/20 }", coretests_fmt_builders_debug_struct_test_multiple_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(51, "Foo {\n    bar: true,\n    baz: 10/20,\n}", coretests_fmt_builders_debug_struct_test_multiple_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_nested_Foo_Debug_fmt is test_nested's Foo's Debug: debug_struct("Foo").field("bar", &true).field("baz", &format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_struct_test_nested_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	std_DebugStruct_field(&builder, "bar", 3, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugStruct_field(&builder, "baz", 3, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugStruct_finish(&builder);
}

/* coretests_fmt_builders_debug_struct_test_nested_Bar_Debug_fmt is test_nested's Bar's Debug: debug_struct("Bar").field("foo", &Foo).field("hello", &"world").finish(). */
static bool
coretests_fmt_builders_debug_struct_test_nested_Bar_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Bar", 3);

	std_DebugStruct_field(&builder, "foo", 3, NULL, coretests_fmt_builders_debug_struct_test_nested_Foo_Debug_fmt);
	std_DebugStruct_field(&builder, "hello", 5, &CORETESTS_FMT_BUILDERS_WORLD, std_str_Debug_fmt);
	return std_DebugStruct_finish(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_nested is debug_struct::test_nested. */
void
test_coretests_fmt_builders_debug_struct_test_nested(void)
{
	coretests_fmt_builders_check(81, "Bar { foo: Foo { bar: true, baz: 10/20 }, hello: \"world\" }", coretests_fmt_builders_debug_struct_test_nested_Bar_Debug_fmt, false);
	coretests_fmt_builders_check(85, "Bar {\n    foo: Foo {\n        bar: true,\n        baz: 10/20,\n    },\n    hello: \"world\",\n}", coretests_fmt_builders_debug_struct_test_nested_Bar_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_empty_non_exhaustive_Foo_Debug_fmt is test_empty_non_exhaustive's Foo's Debug: debug_struct("Foo").finish_non_exhaustive(). */
static bool
coretests_fmt_builders_debug_struct_test_empty_non_exhaustive_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	return std_DebugStruct_finish_non_exhaustive(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_empty_non_exhaustive is debug_struct::test_empty_non_exhaustive. */
void
test_coretests_fmt_builders_debug_struct_test_empty_non_exhaustive(void)
{
	coretests_fmt_builders_check(107, "Foo { .. }", coretests_fmt_builders_debug_struct_test_empty_non_exhaustive_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(108, "Foo { .. }", coretests_fmt_builders_debug_struct_test_empty_non_exhaustive_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive_Foo_Debug_fmt is test_multiple_and_non_exhaustive's Foo's Debug: debug_struct("Foo").field("bar", &true).field("baz", &format_args!(..)).finish_non_exhaustive(). */
static bool
coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	std_DebugStruct_field(&builder, "bar", 3, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugStruct_field(&builder, "baz", 3, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugStruct_finish_non_exhaustive(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive is debug_struct::test_multiple_and_non_exhaustive. */
void
test_coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive(void)
{
	coretests_fmt_builders_check(124, "Foo { bar: true, baz: 10/20, .. }", coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(125, "Foo {\n    bar: true,\n    baz: 10/20,\n    ..\n}", coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Foo_Debug_fmt is test_nested_non_exhaustive's Foo's Debug: debug_struct("Foo").field("bar", &true).field("baz", &format_args!(..)).finish_non_exhaustive(). */
static bool
coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Foo", 3);

	std_DebugStruct_field(&builder, "bar", 3, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugStruct_field(&builder, "baz", 3, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugStruct_finish_non_exhaustive(&builder);
}

/* coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Bar_Debug_fmt is test_nested_non_exhaustive's Bar's Debug: debug_struct("Bar").field("foo", &Foo).field("hello", &"world").finish_non_exhaustive(). */
static bool
coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Bar_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct builder = std_fmt_debug_struct(f, "Bar", 3);

	std_DebugStruct_field(&builder, "foo", 3, NULL, coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Foo_Debug_fmt);
	std_DebugStruct_field(&builder, "hello", 5, &CORETESTS_FMT_BUILDERS_WORLD, std_str_Debug_fmt);
	return std_DebugStruct_finish_non_exhaustive(&builder);
}

/* test_coretests_fmt_builders_debug_struct_test_nested_non_exhaustive is debug_struct::test_nested_non_exhaustive. */
void
test_coretests_fmt_builders_debug_struct_test_nested_non_exhaustive(void)
{
	coretests_fmt_builders_check(159, "Bar { foo: Foo { bar: true, baz: 10/20, .. }, hello: \"world\", .. }", coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Bar_Debug_fmt, false);
	coretests_fmt_builders_check(163, "Bar {\n    foo: Foo {\n        bar: true,\n        baz: 10/20,\n        ..\n    },\n    hello: \"world\",\n    ..\n}", coretests_fmt_builders_debug_struct_test_nested_non_exhaustive_Bar_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_tuple_test_empty_Foo_Debug_fmt is test_empty's Foo's Debug: debug_tuple("Foo").finish(). */
static bool
coretests_fmt_builders_debug_tuple_test_empty_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple builder = std_fmt_debug_tuple(f, "Foo", 3);

	return std_DebugTuple_finish(&builder);
}

/* test_coretests_fmt_builders_debug_tuple_test_empty is debug_tuple::test_empty. */
void
test_coretests_fmt_builders_debug_tuple_test_empty(void)
{
	coretests_fmt_builders_check(206, "Foo", coretests_fmt_builders_debug_tuple_test_empty_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(207, "Foo", coretests_fmt_builders_debug_tuple_test_empty_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_tuple_test_single_Foo_Debug_fmt is test_single's Foo's Debug: debug_tuple("Foo").field(&true).finish(). */
static bool
coretests_fmt_builders_debug_tuple_test_single_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple builder = std_fmt_debug_tuple(f, "Foo", 3);

	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	return std_DebugTuple_finish(&builder);
}

/* test_coretests_fmt_builders_debug_tuple_test_single is debug_tuple::test_single. */
void
test_coretests_fmt_builders_debug_tuple_test_single(void)
{
	coretests_fmt_builders_check(220, "Foo(true)", coretests_fmt_builders_debug_tuple_test_single_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(221, "Foo(\n    true,\n)", coretests_fmt_builders_debug_tuple_test_single_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_tuple_test_multiple_Foo_Debug_fmt is test_multiple's Foo's Debug: debug_tuple("Foo").field(&true).field(&format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_tuple_test_multiple_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple builder = std_fmt_debug_tuple(f, "Foo", 3);

	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugTuple_finish(&builder);
}

/* test_coretests_fmt_builders_debug_tuple_test_multiple is debug_tuple::test_multiple. */
void
test_coretests_fmt_builders_debug_tuple_test_multiple(void)
{
	coretests_fmt_builders_check(239, "Foo(true, 10/20)", coretests_fmt_builders_debug_tuple_test_multiple_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(240, "Foo(\n    true,\n    10/20,\n)", coretests_fmt_builders_debug_tuple_test_multiple_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_tuple_test_nested_Foo_Debug_fmt is test_nested's Foo's Debug: debug_tuple("Foo").field(&true).field(&format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_tuple_test_nested_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple builder = std_fmt_debug_tuple(f, "Foo", 3);

	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugTuple_finish(&builder);
}

/* coretests_fmt_builders_debug_tuple_test_nested_Bar_Debug_fmt is test_nested's Bar's Debug: debug_tuple("Bar").field(&Foo).field(&"world").finish(). */
static bool
coretests_fmt_builders_debug_tuple_test_nested_Bar_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple builder = std_fmt_debug_tuple(f, "Bar", 3);

	std_DebugTuple_field(&builder, NULL, coretests_fmt_builders_debug_tuple_test_nested_Foo_Debug_fmt);
	std_DebugTuple_field(&builder, &CORETESTS_FMT_BUILDERS_WORLD, std_str_Debug_fmt);
	return std_DebugTuple_finish(&builder);
}

/* test_coretests_fmt_builders_debug_tuple_test_nested is debug_tuple::test_nested. */
void
test_coretests_fmt_builders_debug_tuple_test_nested(void)
{
	coretests_fmt_builders_check(267, "Bar(Foo(true, 10/20), \"world\")", coretests_fmt_builders_debug_tuple_test_nested_Bar_Debug_fmt, false);
	coretests_fmt_builders_check(268, "Bar(\n    Foo(\n        true,\n        10/20,\n    ),\n    \"world\",\n)", coretests_fmt_builders_debug_tuple_test_nested_Bar_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_list_test_empty_Foo_Debug_fmt is test_empty's Foo's Debug: debug_list().finish(). */
static bool
coretests_fmt_builders_debug_list_test_empty_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	return std_DebugList_finish(&builder);
}

/* test_coretests_fmt_builders_debug_list_test_empty is debug_list::test_empty. */
void
test_coretests_fmt_builders_debug_list_test_empty(void)
{
	coretests_fmt_builders_check(836, "[]", coretests_fmt_builders_debug_list_test_empty_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(837, "[]", coretests_fmt_builders_debug_list_test_empty_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_list_test_single_Foo_Debug_fmt is test_single's Foo's Debug: debug_list().entry(&true).finish(). */
static bool
coretests_fmt_builders_debug_list_test_single_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* test_coretests_fmt_builders_debug_list_test_single is debug_list::test_single. */
void
test_coretests_fmt_builders_debug_list_test_single(void)
{
	coretests_fmt_builders_check(850, "[true]", coretests_fmt_builders_debug_list_test_single_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(851, "[\n    true,\n]", coretests_fmt_builders_debug_list_test_single_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_list_test_multiple_Foo_Debug_fmt is test_multiple's Foo's Debug: debug_list().entry(&true).entry(&format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_list_test_multiple_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* test_coretests_fmt_builders_debug_list_test_multiple is debug_list::test_multiple. */
void
test_coretests_fmt_builders_debug_list_test_multiple(void)
{
	coretests_fmt_builders_check(869, "[true, 10/20]", coretests_fmt_builders_debug_list_test_multiple_Foo_Debug_fmt, false);
	coretests_fmt_builders_check(870, "[\n    true,\n    10/20,\n]", coretests_fmt_builders_debug_list_test_multiple_Foo_Debug_fmt, true);
}

/* coretests_fmt_builders_debug_list_test_nested_Foo_Debug_fmt is test_nested's Foo's Debug: debug_list().entry(&true).entry(&format_args!(..)).finish(). */
static bool
coretests_fmt_builders_debug_list_test_nested_Foo_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_TRUE, std_bool_Debug_fmt);
	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_TEN_TWENTY, coretests_fmt_builders_Arguments_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* coretests_fmt_builders_debug_list_test_nested_Bar_Debug_fmt is test_nested's Bar's Debug: debug_list().entry(&Foo).entry(&"world").finish(). */
static bool
coretests_fmt_builders_debug_list_test_nested_Bar_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	std_DebugList_entry(&builder, NULL, coretests_fmt_builders_debug_list_test_nested_Foo_Debug_fmt);
	std_DebugList_entry(&builder, &CORETESTS_FMT_BUILDERS_WORLD, std_str_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* test_coretests_fmt_builders_debug_list_test_nested is debug_list::test_nested. */
void
test_coretests_fmt_builders_debug_list_test_nested(void)
{
	coretests_fmt_builders_check(897, "[[true, 10/20], \"world\"]", coretests_fmt_builders_debug_list_test_nested_Bar_Debug_fmt, false);
	coretests_fmt_builders_check(898, "[\n    [\n        true,\n        10/20,\n    ],\n    \"world\",\n]", coretests_fmt_builders_debug_list_test_nested_Bar_Debug_fmt, true);
}
