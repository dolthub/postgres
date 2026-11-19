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
 * What a libc locale's stored name asks of the vendored collator
 * (doltgres' locale.rs, design item 12): the same answer on every platform,
 * from the name's form and the tree's table of Windows' names, never from an
 * operating system.
 */
#ifndef DOLTGRES_DOLTGRES_LOCALE_H
#define DOLTGRES_DOLTGRES_LOCALE_H

#include "doltgres/std/str.h"

struct StdFormatter;

/* DoltgresLocalePartTag is Part's variants. */
typedef enum DoltgresLocalePartTag
{
	DOLTGRES_LOCALE_PART_MODIFIER,
	DOLTGRES_LOCALE_PART_SORT,
} DoltgresLocalePartTag;

/*
 * DoltgresLocalePart is Part: a part of a libc locale's name that its tag
 * holds nothing for, a POSIX name's modifier or the sort name Windows puts
 * after a language tag.
 */
typedef struct DoltgresLocalePart
{
	DoltgresLocalePartTag tag;
	union
	{
		StdStr		modifier;
		StdStr		sort;
	}			u;
} DoltgresLocalePart;

/* DoltgresLocaleRulesTag is Rules' variants. */
typedef enum DoltgresLocaleRulesTag
{
	DOLTGRES_LOCALE_RULES_C,
	DOLTGRES_LOCALE_RULES_C_UTF8,
	DOLTGRES_LOCALE_RULES_TAG,
	DOLTGRES_LOCALE_RULES_UNKNOWN,
} DoltgresLocaleRulesTag;

/*
 * DoltgresLocaleRules is Rules: what orders and classifies text under a
 * libc locale's stored name, byte order with C's or Unicode's classes, the
 * collator's rules for a language tag with the part of the name the tag
 * holds nothing for, or a name of no form.
 */
typedef struct DoltgresLocaleRules
{
	DoltgresLocaleRulesTag tag;
	union
	{
		struct
		{
			StdCowStr	tag;
			bool		has_unknown;
			DoltgresLocalePart unknown;
		}			tag;
	}			u;
} DoltgresLocaleRules;

/* DoltgresLocaleClasses is Classes: what classifies a locale's text and maps its case, as its ctype name asks. */
typedef enum DoltgresLocaleClasses
{
	DOLTGRES_LOCALE_CLASSES_COLLATOR,
	DOLTGRES_LOCALE_CLASSES_C,
	DOLTGRES_LOCALE_CLASSES_C_UTF8,
} DoltgresLocaleClasses;

/* DoltgresLocaleOpenedTag is Opened's variants. */
typedef enum DoltgresLocaleOpenedTag
{
	DOLTGRES_LOCALE_OPENED_STOCK,
	DOLTGRES_LOCALE_OPENED_BUILTIN,
	DOLTGRES_LOCALE_OPENED_COLLATOR,
} DoltgresLocaleOpenedTag;

/*
 * DoltgresLocaleOpened is Opened: what the libc site builds for a locale's
 * two stored names, Postgres' own libc locale, its builtin one, or its ICU
 * one, with whether the collation half is marked as C's, what the ctype half
 * classifies by, and whether the constructor is asked a second time.
 */
typedef struct DoltgresLocaleOpened
{
	DoltgresLocaleOpenedTag tag;
	union
	{
		struct
		{
			bool		collate_c;
			DoltgresLocaleClasses classes;
			bool		second;
		}			collator;
	}			u;
} DoltgresLocaleOpened;

extern bool doltgres_locale_Rules_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool doltgres_locale_Rules_PartialEq_eq(const DoltgresLocaleRules *self, const DoltgresLocaleRules *other);
extern void doltgres_locale_Rules_drop(DoltgresLocaleRules *self);
extern bool doltgres_locale_Part_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool doltgres_locale_Part_PartialEq_eq(const DoltgresLocalePart *self, const DoltgresLocalePart *other);
extern bool doltgres_locale_Classes_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool doltgres_locale_Opened_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool doltgres_locale_Opened_PartialEq_eq(const DoltgresLocaleOpened *self, const DoltgresLocaleOpened *other);
extern DoltgresLocaleOpened doltgres_locale_opened(StdStr collate, StdStr ctype);
extern void doltgres_locale_asked(StdStr collate, StdStr ctype, bool second, StdStr *result_0,
								  DoltgresLocaleRules *result_1);
extern DoltgresLocaleRules doltgres_locale_named(StdStr name);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_locale_tests_the_c_locales_sort_by_bytes(void);
extern void test_doltgres_locale_tests_a_posix_name_is_its_language_and_territory(void);
extern void test_doltgres_locale_tests_a_modifier_is_the_subtag_it_names(void);
extern void test_doltgres_locale_tests_a_posix_name_may_state_its_script(void);
extern void test_doltgres_locale_tests_a_language_tag_is_itself(void);
extern void test_doltgres_locale_tests_a_sort_name_is_its_rows_tag_or_unknown(void);
extern void test_doltgres_locale_tests_every_sort_is_one_icu_maps(void);
extern void test_doltgres_locale_tests_a_windows_name_is_its_rows_tag(void);
extern void test_doltgres_locale_tests_a_glibc_alias_is_its_locales_tag(void);
extern void test_doltgres_locale_tests_the_names_postgres_maps_are_rows(void);
extern void test_doltgres_locale_tests_the_names_of_earlier_windows_releases_are_rows(void);
extern void test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name(void);
extern void test_doltgres_locale_tests_the_c_family_is_postgres_own(void);
extern void test_doltgres_locale_tests_a_c_utf8_ctype_takes_the_builtin_classes(void);
extern void test_doltgres_locale_tests_a_named_half_is_the_collators(void);
extern void test_doltgres_locale_tests_two_tags_are_two_asks(void);
extern void test_doltgres_locale_tests_a_name_of_no_form_is_unknown(void);
extern void test_doltgres_locale_tests_every_row_is_found_under_its_name(void);
#endif

#endif							/* DOLTGRES_DOLTGRES_LOCALE_H */
