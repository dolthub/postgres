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
 * from the name's form and the tree's tables of names (glibc's aliases,
 * Windows' traditional names, and Windows' sort names), compiled in from
 * src/doltgres/icu as the Rust's include_str! compiles them in, never from
 * an operating system.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/doltgres/locale.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unicode.h"
#endif
#include "locale_tables.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "locale_tests.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "locale.rs"

/* DOLTGRES_LOCALE_TEXT is the str of a compiled-in table's bytes. */
#define DOLTGRES_LOCALE_TEXT(bytes) ((StdStr) {(const char *) (bytes), sizeof(bytes)})

/*
 * DOLTGRES_LOCALE_WINDOWS is WINDOWS: Windows' traditional locale names, each
 * followed by a tab and its language tag.
 */
#define DOLTGRES_LOCALE_WINDOWS DOLTGRES_LOCALE_TEXT(DOLTGRES_LOCALE_WINDOWS_BYTES)

/*
 * DOLTGRES_LOCALE_SORTS is SORTS: Windows' locale names with a sort name
 * that ICU has a collation for, each followed by a tab, its Windows locale
 * id, ICU's locale for that id, and its language tag.
 */
#define DOLTGRES_LOCALE_SORTS DOLTGRES_LOCALE_TEXT(DOLTGRES_LOCALE_SORTS_BYTES)

/* DOLTGRES_LOCALE_GLIBC is GLIBC: glibc's locale aliases, each followed by a tab and its language tag. */
#define DOLTGRES_LOCALE_GLIBC DOLTGRES_LOCALE_TEXT(DOLTGRES_LOCALE_GLIBC_BYTES)

/* DOLTGRES_LOCALE_NONE is the None of an Option<&str>. */
#define DOLTGRES_LOCALE_NONE ((StdStr) {NULL, 0})

/*
 * doltgres_locale_split_t is str::Split of a char: the text not yet split,
 * the char, and whether the last piece was given.
 */
typedef struct doltgres_locale_split_t
{
	StdStr		rest;
	char		mark;
	bool		finished;
} doltgres_locale_split_t;

/* doltgres_locale_rows_t is rows' iterator: the table's text not yet read. */
typedef struct doltgres_locale_rows_t
{
	StdStr		rest;
} doltgres_locale_rows_t;

/*
 * doltgres_locale_named_frame_t is the landing frame of a name's rules read
 * while another's are held: the held rules, which an unwind drops, the name,
 * and its rules.
 */
typedef struct doltgres_locale_named_frame_t
{
	DoltgresLocaleRules *held;
	StdStr		name;
	DoltgresLocaleRules result;
} doltgres_locale_named_frame_t;

static bool doltgres_locale_posix(StdStr name, DoltgresLocaleRules *result);
static bool doltgres_locale_field(StdStr table, StdStr name, size_t after, StdStr *result);
static bool doltgres_locale_tagged(StdStr name, DoltgresLocaleRules *result);
static bool doltgres_locale_windows(StdStr name, DoltgresLocaleRules *result);

/* doltgres_locale_eq is str's ==. */
static bool
doltgres_locale_eq(StdStr a, StdStr b)
{
	return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

/* doltgres_locale_eq_ignore_ascii_case is str::eq_ignore_ascii_case: the same bytes but for ASCII letters' case. */
static bool
doltgres_locale_eq_ignore_ascii_case(StdStr a, StdStr b)
{
	if (a.len != b.len)
		return false;
	for (size_t i = 0; i < a.len; i++)
		if (pg_ascii_tolower((unsigned char) a.ptr[i]) != pg_ascii_tolower((unsigned char) b.ptr[i]))
			return false;
	return true;
}

/* doltgres_locale_is_empty_some is whether an Option<&str> is Some(""). */
static bool
doltgres_locale_is_empty_some(StdStr str)
{
	return str.ptr != NULL && str.len == 0;
}

/* doltgres_locale_lower is u8::is_ascii_lowercase. */
static bool
doltgres_locale_lower(unsigned char c)
{
	return c >= 'a' && c <= 'z';
}

/* doltgres_locale_upper is u8::is_ascii_uppercase. */
static bool
doltgres_locale_upper(unsigned char c)
{
	return c >= 'A' && c <= 'Z';
}

/* doltgres_locale_alphabetic is u8::is_ascii_alphabetic. */
static bool
doltgres_locale_alphabetic(unsigned char c)
{
	return doltgres_locale_lower(c) || doltgres_locale_upper(c);
}

/* doltgres_locale_alphanumeric is u8::is_ascii_alphanumeric. */
static bool
doltgres_locale_alphanumeric(unsigned char c)
{
	return doltgres_locale_alphabetic(c) || (c >= '0' && c <= '9');
}

/* doltgres_locale_all returns whether every byte of a text is of a class. */
static bool
doltgres_locale_all(StdStr str, bool (*is) (unsigned char c))
{
	for (size_t i = 0; i < str.len; i++)
		if (!is((unsigned char) str.ptr[i]))
			return false;
	return true;
}

/* doltgres_locale_slice returns the bytes of a text from a start to an end, which the caller has checked. */
static StdStr
doltgres_locale_slice(StdStr str, size_t start, size_t end)
{
	StdStr		slice = {str.ptr + start, end - start};

	return slice;
}

/* doltgres_locale_split returns str::split of a char over a text. */
static doltgres_locale_split_t
doltgres_locale_split(StdStr str, char mark)
{
	doltgres_locale_split_t split = {.rest = str, .mark = mark};

	return split;
}

/*
 * doltgres_locale_cut splits a name at a mark into what stands before it and,
 * when the mark is there, what follows it.
 */
static StdStr
doltgres_locale_cut(StdStr name, char mark, StdStr *after)
{
	const char *at = memchr(name.ptr, mark, name.len);
	StdStr		before = name;

	*after = DOLTGRES_LOCALE_NONE;
	if (at == NULL)
		return before;
	before.len = at - name.ptr;
	after->ptr = at + 1;
	after->len = name.len - before.len - 1;
	return before;
}

/* doltgres_locale_split_next is Split's next: the text up to the next mark, or the rest after the last one. */
static bool
doltgres_locale_split_next(doltgres_locale_split_t *self, StdStr *piece)
{
	StdStr		after;

	if (self->finished)
		return false;
	*piece = doltgres_locale_cut(self->rest, self->mark, &after);
	if (after.ptr == NULL)
		self->finished = true;
	else
		self->rest = after;
	return true;
}

/* doltgres_locale_tag_of returns the tag the collator is opened with under a name's rules, or None. */
static StdStr
doltgres_locale_tag_of(const DoltgresLocaleRules *rules)
{
	switch (rules->tag)
	{
		case DOLTGRES_LOCALE_RULES_TAG:
			return std_cow_str_deref(&rules->u.tag.tag);
		case DOLTGRES_LOCALE_RULES_UNKNOWN:
			return STD_STR("und");
		case DOLTGRES_LOCALE_RULES_C:
		case DOLTGRES_LOCALE_RULES_C_UTF8:
			return DOLTGRES_LOCALE_NONE;
	}
	pg_unreachable();
}

/* doltgres_locale_postgres_own returns whether a name is `C` or `POSIX` itself. */
static bool
doltgres_locale_postgres_own(StdStr name)
{
	return doltgres_locale_eq(name, STD_STR("C")) || doltgres_locale_eq(name, STD_STR("POSIX"));
}

/* doltgres_locale_named_beside_body reads the frame's name's rules. */
static void
doltgres_locale_named_beside_body(void *frame)
{
	doltgres_locale_named_frame_t *f = frame;

	f->result = doltgres_locale_named(f->name);
}

/* doltgres_locale_named_beside_pad drops the held rules of an unwound read. */
static void
doltgres_locale_named_beside_pad(void *frame)
{
	doltgres_locale_named_frame_t *f = frame;

	doltgres_locale_Rules_drop(f->held);
}

/* doltgres_locale_named_beside returns a name's rules while another name's rules are held, which an unwind drops. */
static DoltgresLocaleRules
doltgres_locale_named_beside(DoltgresLocaleRules *held, StdStr name)
{
	doltgres_locale_named_frame_t frame = {.held = held, .name = name};

	std_landing(&frame, doltgres_locale_named_beside_body, doltgres_locale_named_beside_pad);
	return frame.result;
}

/* doltgres_locale_Rules_Debug_fmt is Rules' derived Debug. */
bool
doltgres_locale_Rules_Debug_fmt(const void *value, StdFormatter *f)
{
	const DoltgresLocaleRules *self = value;
	StdStr		tag;
	StdOptionRef unknown;
	StdDebugStruct s;

	switch (self->tag)
	{
		case DOLTGRES_LOCALE_RULES_C:
			return std_fmt_write_str(f, "C", 1);
		case DOLTGRES_LOCALE_RULES_C_UTF8:
			return std_fmt_write_str(f, "CUtf8", 5);
		case DOLTGRES_LOCALE_RULES_TAG:
			tag = std_cow_str_deref(&self->u.tag.tag);
			unknown.has = self->u.tag.has_unknown;
			unknown.some.value = &self->u.tag.unknown;
			unknown.some.fmt = doltgres_locale_Part_Debug_fmt;
			s = std_fmt_debug_struct(f, "Tag", 3);
			std_DebugStruct_field(&s, "tag", 3, &tag, std_str_Debug_fmt);
			std_DebugStruct_field(&s, "unknown", 7, &unknown, std_option_Debug_fmt);
			return std_DebugStruct_finish(&s);
		case DOLTGRES_LOCALE_RULES_UNKNOWN:
			return std_fmt_write_str(f, "Unknown", 7);
	}
	pg_unreachable();
}

/* doltgres_locale_Rules_PartialEq_eq is Rules' derived PartialEq: the same variant with equal fields. */
bool
doltgres_locale_Rules_PartialEq_eq(const DoltgresLocaleRules *self, const DoltgresLocaleRules *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag != DOLTGRES_LOCALE_RULES_TAG)
		return true;
	return doltgres_locale_eq(std_cow_str_deref(&self->u.tag.tag), std_cow_str_deref(&other->u.tag.tag)) &&
		self->u.tag.has_unknown == other->u.tag.has_unknown &&
		(!self->u.tag.has_unknown || doltgres_locale_Part_PartialEq_eq(&self->u.tag.unknown, &other->u.tag.unknown));
}

/* doltgres_locale_Rules_drop frees a tag's owned text. */
void
doltgres_locale_Rules_drop(DoltgresLocaleRules *self)
{
	if (self->tag == DOLTGRES_LOCALE_RULES_TAG)
		std_CowStr_drop(&self->u.tag.tag);
}

/* doltgres_locale_Part_Debug_fmt is Part's derived Debug. */
bool
doltgres_locale_Part_Debug_fmt(const void *value, StdFormatter *f)
{
	const DoltgresLocalePart *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case DOLTGRES_LOCALE_PART_MODIFIER:
			t = std_fmt_debug_tuple(f, "Modifier", 8);
			std_DebugTuple_field(&t, &self->u.modifier, std_str_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case DOLTGRES_LOCALE_PART_SORT:
			t = std_fmt_debug_tuple(f, "Sort", 4);
			std_DebugTuple_field(&t, &self->u.sort, std_str_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* doltgres_locale_Part_PartialEq_eq is Part's derived PartialEq: the same variant with equal texts. */
bool
doltgres_locale_Part_PartialEq_eq(const DoltgresLocalePart *self, const DoltgresLocalePart *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == DOLTGRES_LOCALE_PART_MODIFIER)
		return doltgres_locale_eq(self->u.modifier, other->u.modifier);
	return doltgres_locale_eq(self->u.sort, other->u.sort);
}

/* doltgres_locale_Classes_Debug_fmt is Classes' derived Debug. */
bool
doltgres_locale_Classes_Debug_fmt(const void *value, StdFormatter *f)
{
	switch (*(const DoltgresLocaleClasses *) value)
	{
		case DOLTGRES_LOCALE_CLASSES_COLLATOR:
			return std_fmt_write_str(f, "Collator", 8);
		case DOLTGRES_LOCALE_CLASSES_C:
			return std_fmt_write_str(f, "C", 1);
		case DOLTGRES_LOCALE_CLASSES_C_UTF8:
			return std_fmt_write_str(f, "CUtf8", 5);
	}
	pg_unreachable();
}

/* doltgres_locale_Opened_Debug_fmt is Opened's derived Debug. */
bool
doltgres_locale_Opened_Debug_fmt(const void *value, StdFormatter *f)
{
	const DoltgresLocaleOpened *self = value;
	StdDebugStruct s;

	switch (self->tag)
	{
		case DOLTGRES_LOCALE_OPENED_STOCK:
			return std_fmt_write_str(f, "Stock", 5);
		case DOLTGRES_LOCALE_OPENED_BUILTIN:
			return std_fmt_write_str(f, "Builtin", 7);
		case DOLTGRES_LOCALE_OPENED_COLLATOR:
			s = std_fmt_debug_struct(f, "Collator", 8);
			std_DebugStruct_field(&s, "collate_c", 9, &self->u.collator.collate_c, std_bool_Debug_fmt);
			std_DebugStruct_field(&s, "classes", 7, &self->u.collator.classes, doltgres_locale_Classes_Debug_fmt);
			std_DebugStruct_field(&s, "second", 6, &self->u.collator.second, std_bool_Debug_fmt);
			return std_DebugStruct_finish(&s);
	}
	pg_unreachable();
}

/* doltgres_locale_Opened_PartialEq_eq is Opened's derived PartialEq: the same variant with equal fields. */
bool
doltgres_locale_Opened_PartialEq_eq(const DoltgresLocaleOpened *self, const DoltgresLocaleOpened *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag != DOLTGRES_LOCALE_OPENED_COLLATOR)
		return true;
	return self->u.collator.collate_c == other->u.collator.collate_c &&
		self->u.collator.classes == other->u.collator.classes &&
		self->u.collator.second == other->u.collator.second;
}

/*
 * doltgres_locale_opened returns what the libc site builds for a locale's
 * collate and ctype names: Postgres' own libc locale for two names of `C` or
 * `POSIX`, the builtin one for any other pair that both sort by bytes, and
 * otherwise the collator, marked where a half asks for byte order or C's
 * classes, and asked twice when the two names' tags differ.
 */
DoltgresLocaleOpened
doltgres_locale_opened(StdStr collate, StdStr ctype)
{
	DoltgresLocaleRules ordered = doltgres_locale_named(collate);
	DoltgresLocaleRules classed = doltgres_locale_named_beside(&ordered, ctype);
	StdStr		order = doltgres_locale_tag_of(&ordered);
	StdStr		class = doltgres_locale_tag_of(&classed);
	DoltgresLocaleOpened opened;

	if (order.ptr == NULL && class.ptr == NULL)
		opened.tag = doltgres_locale_postgres_own(collate) && doltgres_locale_postgres_own(ctype) ?
			DOLTGRES_LOCALE_OPENED_STOCK : DOLTGRES_LOCALE_OPENED_BUILTIN;
	else
	{
		opened.tag = DOLTGRES_LOCALE_OPENED_COLLATOR;
		opened.u.collator.collate_c = order.ptr == NULL;
		if (class.ptr != NULL)
			opened.u.collator.classes = DOLTGRES_LOCALE_CLASSES_COLLATOR;
		else if (classed.tag == DOLTGRES_LOCALE_RULES_C_UTF8)
			opened.u.collator.classes = DOLTGRES_LOCALE_CLASSES_C_UTF8;
		else
			opened.u.collator.classes = DOLTGRES_LOCALE_CLASSES_C;
		opened.u.collator.second = order.ptr != NULL && class.ptr != NULL && !doltgres_locale_eq(order, class);
	}
	doltgres_locale_Rules_drop(&classed);
	doltgres_locale_Rules_drop(&ordered);
	return opened;
}

/*
 * doltgres_locale_asked returns the name the ICU constructor is built from
 * when the libc site asks it, with its rules: the ctype name on the second
 * ask and where the collate name asks for byte order, and the collate name
 * otherwise.
 */
void
doltgres_locale_asked(StdStr collate, StdStr ctype, bool second, StdStr *result_0, DoltgresLocaleRules *result_1)
{
	DoltgresLocaleRules ordered = doltgres_locale_named(collate);

	if (second || doltgres_locale_tag_of(&ordered).ptr == NULL)
	{
		*result_0 = ctype;
		*result_1 = doltgres_locale_named_beside(&ordered, ctype);
		doltgres_locale_Rules_drop(&ordered);
		return;
	}
	*result_0 = collate;
	*result_1 = ordered;
}

/* doltgres_locale_borrowed returns a language tag's rules with nothing unknown beside it. */
static DoltgresLocaleRules
doltgres_locale_borrowed(StdStr tag)
{
	DoltgresLocaleRules rules = {.tag = DOLTGRES_LOCALE_RULES_TAG};

	rules.u.tag.tag.tag = STD_COW_STR_BORROWED;
	rules.u.tag.tag.u.borrowed = tag;
	return rules;
}

/*
 * doltgres_locale_named returns what a libc locale's stored name asks for,
 * by the first of its forms that fits: `C`, `POSIX`, or `C.encoding`; one of
 * glibc's aliases, whose tag wins over a POSIX name's and Windows' where they
 * differ (design item 12); POSIX's `ll_TT.encoding@modifier`; a language
 * tag, which Windows may follow with a sort name; and Windows'
 * `Language_Country.codepage`.
 */
DoltgresLocaleRules
doltgres_locale_named(StdStr name)
{
	DoltgresLocaleRules rules = {0};
	StdStr		tag;

	if (doltgres_locale_eq(name, STD_STR("C")) || doltgres_locale_eq(name, STD_STR("POSIX")))
	{
		rules.tag = DOLTGRES_LOCALE_RULES_C;
		return rules;
	}
	if (name.len >= 2 && memcmp(name.ptr, "C.", 2) == 0)
	{
		StdStr		encoding = {name.ptr + 2, name.len - 2};
		bool		unicode = doltgres_locale_eq_ignore_ascii_case(encoding, STD_STR("UTF-8")) ||
			doltgres_locale_eq_ignore_ascii_case(encoding, STD_STR("utf8"));

		rules.tag = unicode ? DOLTGRES_LOCALE_RULES_C_UTF8 : DOLTGRES_LOCALE_RULES_C;
		return rules;
	}
	if (doltgres_locale_field(DOLTGRES_LOCALE_GLIBC, name, 0, &tag))
		return doltgres_locale_borrowed(tag);
	if (doltgres_locale_posix(name, &rules) || doltgres_locale_tagged(name, &rules) ||
		doltgres_locale_windows(name, &rules))
		return rules;
	rules.tag = DOLTGRES_LOCALE_RULES_UNKNOWN;
	return rules;
}

/*
 * doltgres_locale_posix answers what a POSIX name asks for, or false for a
 * name of another form: `ll_TT` is the tag `ll-TT`, with the script between
 * them where the name has one (`ll_Ssss_TT`), and a modifier is the script
 * or variant it names, nothing for `@euro`, and unknown otherwise or beside
 * a script the name already has.
 */
static bool
doltgres_locale_posix(StdStr name, DoltgresLocaleRules *result)
{
	StdStr		modifier;
	StdStr		encoding;
	StdStr		rest = doltgres_locale_cut(doltgres_locale_cut(name, '@', &modifier), '.', &encoding);
	doltgres_locale_split_t parts = doltgres_locale_split(rest, '_');
	StdStr		language;
	StdStr		pieces[3] = {DOLTGRES_LOCALE_NONE, DOLTGRES_LOCALE_NONE, DOLTGRES_LOCALE_NONE};
	StdStr		script = DOLTGRES_LOCALE_NONE;
	StdStr		territory = DOLTGRES_LOCALE_NONE;
	StdStr		named = DOLTGRES_LOCALE_NONE;
	StdStr		variant = DOLTGRES_LOCALE_NONE;
	StdStr		subtags[4];
	bool		fits;

	(void) doltgres_locale_split_next(&parts, &language);
	for (int i = 0; i < 3; i++)
		(void) doltgres_locale_split_next(&parts, &pieces[i]);
	if (pieces[1].ptr == NULL)
		territory = pieces[0];
	else if (pieces[2].ptr == NULL)
	{
		script = pieces[0];
		territory = pieces[1];
	}
	else
		return false;
	fits = language.len >= 2 && language.len <= 3 && doltgres_locale_all(language, doltgres_locale_lower) &&
		(script.ptr == NULL ||
		 (script.len == 4 && doltgres_locale_upper((unsigned char) script.ptr[0]) &&
		  doltgres_locale_all(doltgres_locale_slice(script, 1, 4), doltgres_locale_lower))) &&
		(territory.ptr == NULL || (territory.len == 2 && doltgres_locale_all(territory, doltgres_locale_upper))) &&
		!doltgres_locale_is_empty_some(encoding) && !doltgres_locale_is_empty_some(modifier);
	if (!fits)
		return false;
	*result = doltgres_locale_borrowed(language);
	if (modifier.ptr != NULL && doltgres_locale_eq(modifier, STD_STR("latin")) && script.ptr == NULL)
		named = STD_STR("Latn");
	else if (modifier.ptr != NULL && doltgres_locale_eq(modifier, STD_STR("cyrillic")) && script.ptr == NULL)
		named = STD_STR("Cyrl");
	else if (modifier.ptr != NULL && doltgres_locale_eq(modifier, STD_STR("valencia")))
		variant = STD_STR("valencia");
	else if (modifier.ptr != NULL && !doltgres_locale_eq(modifier, STD_STR("euro")))
	{
		result->u.tag.has_unknown = true;
		result->u.tag.unknown.tag = DOLTGRES_LOCALE_PART_MODIFIER;
		result->u.tag.unknown.u.modifier = modifier;
	}
	subtags[0] = language;
	subtags[1] = script.ptr != NULL ? script : named;
	subtags[2] = territory;
	subtags[3] = variant;
	if (subtags[1].ptr != NULL || territory.ptr != NULL || variant.ptr != NULL)
	{
		StdString	tag;
		size_t		len = 0;
		size_t		count = 0;
		bool		first = true;

		for (int i = 0; i < 4; i++)
			if (subtags[i].ptr != NULL)
			{
				len += subtags[i].len;
				count++;
			}
		std_vec_with_capacity_Checked(&tag, len + count - 1, 1, 1);
		for (int i = 0; i < 4; i++)
			if (subtags[i].ptr != NULL)
			{
				if (!first)
					std_string_push_str_Checked(&tag, "-", 1);
				first = false;
				std_string_push_str_Checked(&tag, subtags[i].ptr, subtags[i].len);
			}
		result->u.tag.tag.tag = STD_COW_STR_OWNED;
		result->u.tag.tag.u.owned = tag;
	}
	return true;
}

/* doltgres_locale_rows returns the rows of one of the tree's tables of names. */
static doltgres_locale_rows_t
doltgres_locale_rows(StdStr table)
{
	doltgres_locale_rows_t rows = {.rest = table};

	return rows;
}

/*
 * doltgres_locale_rows_next answers the next row of a table's text, its
 * line's fields split at tabs, passing over comment lines and empty ones, as
 * str::lines splits the text into lines.
 */
static bool
doltgres_locale_rows_next(doltgres_locale_rows_t *self, doltgres_locale_split_t *row)
{
	while (self->rest.len > 0)
	{
		const char *end = memchr(self->rest.ptr, '\n', self->rest.len);
		StdStr		line = self->rest;

		if (end == NULL)
			self->rest.len = 0;
		else
		{
			line.len = end - line.ptr;
			self->rest.ptr = end + 1;
			self->rest.len -= line.len + 1;
			if (line.len > 0 && line.ptr[line.len - 1] == '\r')
				line.len--;
		}
		if (line.len == 0 || line.ptr[0] == '#')
			continue;
		*row = doltgres_locale_split(line, '\t');
		return true;
	}
	return false;
}

/*
 * doltgres_locale_field answers a field of the first row of a table that a
 * name heads and that has that field, whatever the case of the name's
 * letters: the field that many fields after the name.
 */
static bool
doltgres_locale_field(StdStr table, StdStr name, size_t after, StdStr *result)
{
	doltgres_locale_rows_t rows = doltgres_locale_rows(table);
	doltgres_locale_split_t row;

	while (doltgres_locale_rows_next(&rows, &row))
	{
		StdStr		first;

		(void) doltgres_locale_split_next(&row, &first);
		if (!doltgres_locale_eq_ignore_ascii_case(first, name))
			continue;
		for (size_t i = 0; i < after; i++)
			(void) doltgres_locale_split_next(&row, &first);
		if (doltgres_locale_split_next(&row, result))
			return true;
	}
	return false;
}

/* doltgres_locale_tagged_closure_0 is tagged's word: one to eight ASCII letters and digits. */
static bool
doltgres_locale_tagged_closure_0(StdStr str)
{
	return str.len >= 1 && str.len <= 8 && doltgres_locale_all(str, doltgres_locale_alphanumeric);
}

/*
 * doltgres_locale_tagged answers what a language tag asks for, alone or
 * followed by one of Windows' sort names, or false for a name of another
 * form: the tag itself, or the sorts' table's tag for the two together, and
 * otherwise the tag with the sort name unknown beside it.
 */
static bool
doltgres_locale_tagged(StdStr name, DoltgresLocaleRules *result)
{
	StdStr		encoding;
	StdStr		rest = doltgres_locale_cut(name, '.', &encoding);
	StdStr		sort;
	StdStr		tag = doltgres_locale_cut(rest, '_', &sort);
	StdStr		subtags;
	StdStr		language = doltgres_locale_cut(tag, '-', &subtags);
	doltgres_locale_split_t words;
	StdStr		word;
	StdStr		sorted;
	bool		fits;

	if (subtags.ptr == NULL)
		return false;
	fits = doltgres_locale_eq_ignore_ascii_case(language, STD_STR("x")) ||
		(language.len >= 2 && language.len <= 3 && doltgres_locale_all(language, doltgres_locale_alphabetic));
	words = doltgres_locale_split(subtags, '-');
	while (fits && doltgres_locale_split_next(&words, &word))
		fits = doltgres_locale_tagged_closure_0(word);
	fits = fits && (sort.ptr == NULL || doltgres_locale_tagged_closure_0(sort)) &&
		!doltgres_locale_is_empty_some(encoding);
	if (!fits)
		return false;
	if (doltgres_locale_field(DOLTGRES_LOCALE_SORTS, rest, 2, &sorted))
	{
		*result = doltgres_locale_borrowed(sorted);
		return true;
	}
	*result = doltgres_locale_borrowed(tag);
	if (sort.ptr != NULL)
	{
		result->u.tag.has_unknown = true;
		result->u.tag.unknown.tag = DOLTGRES_LOCALE_PART_SORT;
		result->u.tag.unknown.u.sort = sort;
	}
	return true;
}

/*
 * doltgres_locale_windows answers what one of Windows' traditional names
 * asks for, or false for a name with no row: its row's tag, for the whole
 * name or for what stands before its last dot, which a code page follows.
 */
static bool
doltgres_locale_windows(StdStr name, DoltgresLocaleRules *result)
{
	StdStr		tag;
	size_t		dot = name.len;

	if (doltgres_locale_field(DOLTGRES_LOCALE_WINDOWS, name, 0, &tag))
	{
		*result = doltgres_locale_borrowed(tag);
		return true;
	}
	while (dot > 0 && name.ptr[dot - 1] != '.')
		dot--;
	if (dot == 0 || dot == name.len ||
		!doltgres_locale_field(DOLTGRES_LOCALE_WINDOWS, doltgres_locale_slice(name, 0, dot - 1), 0, &tag))
		return false;
	*result = doltgres_locale_borrowed(tag);
	return true;
}

#ifdef DOLTGRES_TEST_DOLTGRES

/*
 * DOLTGRES_LOCALE_TESTS_LOCMAP is the vendored ICU's common/locmap.cpp, its
 * table from a Windows locale id to its locale.
 */
#define DOLTGRES_LOCALE_TESTS_LOCMAP DOLTGRES_LOCALE_TEXT(DOLTGRES_LOCALE_TESTS_LOCMAP_BYTES)

/* Chars::next's checked instance, which takes the whitespace out of ICU's table, as the tests' codegen of it checks. */
#define STD_STR_SCOPE static
#define STD_STR_DECLARE
#define STD_STR_DEFINE
#define STD_STR_UB_CHECKS 1
#define STD_STR_SUFFIX Checked
#include "doltgres/std/str_template.h"

/* The element layout of the tests' vectors of lines. */
#define DOLTGRES_LOCALE_TESTS_LINE sizeof(StdStr), _Alignof(StdStr)

/* doltgres_locale_tests_option_eq is Option<&str>'s ==. */
static bool
doltgres_locale_tests_option_eq(StdStr a, StdStr b)
{
	return (a.ptr == NULL) == (b.ptr == NULL) && (a.ptr == NULL || doltgres_locale_eq(a, b));
}

/* doltgres_locale_tests_c returns a str of a C string, or None for NULL. */
static StdStr
doltgres_locale_tests_c(const char *literal)
{
	StdStr		str = {literal, literal == NULL ? 0 : strlen(literal)};

	return str;
}

/*
 * doltgres_locale_tests_parts answers what a name that stands for a tag
 * asks for: the tag, which the caller frees, and the part the tag holds
 * nothing for, or None.
 */
static void
doltgres_locale_tests_parts(StdStr name, StdString *tag, StdStr *unknown)
{
	DoltgresLocaleRules rules = doltgres_locale_named(name);

	if (rules.tag != DOLTGRES_LOCALE_RULES_TAG)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&rules, doltgres_locale_Rules_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 244, 22, &message);
	}
	std_CowStr_into_owned(&rules.u.tag.tag, tag);
	*unknown = DOLTGRES_LOCALE_NONE;
	if (rules.u.tag.has_unknown)
		*unknown = rules.u.tag.unknown.tag == DOLTGRES_LOCALE_PART_MODIFIER ?
			rules.u.tag.unknown.u.modifier : rules.u.tag.unknown.u.sort;
}

/* doltgres_locale_tests_message_t is an assert's message of one str's Display, `"{}"`. */
typedef struct doltgres_locale_tests_message_t
{
	StdFmtArg	arg;
	StdFmtArguments args;
} doltgres_locale_tests_message_t;

/* doltgres_locale_tests_message returns the arguments of a message of a str, or NULL for none. */
static const StdFmtArguments *
doltgres_locale_tests_message(doltgres_locale_tests_message_t *message, const StdStr *str)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};

	if (str == NULL)
		return NULL;
	message->arg.value = str;
	message->arg.fmt = std_str_Display_fmt;
	message->args.pieces = pieces;
	message->args.pieces_len = lengthof(pieces);
	message->args.args = &message->arg;
	return &message->args;
}

/* doltgres_locale_tests_panic is assert!'s panic at a line and column, its message a str. */
pg_noreturn static void
doltgres_locale_tests_panic(uint32 line, uint32 col, StdStr str)
{
	doltgres_locale_tests_message_t message;

	std_panic_fmt(RUST_FILE, line, col, doltgres_locale_tests_message(&message, &str));
}

/*
 * doltgres_locale_tests_parts_t is an assert_eq! of parts' answer for a
 * name: its line and column, the answer, the other name whose answer it
 * should equal or None, that answer once held, the answer it should be, and
 * the str its message shows, NULL for none.
 */
typedef struct doltgres_locale_tests_parts_t
{
	uint32		line;
	uint32		col;
	StdString	tag;
	StdStr		unknown;
	StdStr		other;
	bool		other_live;
	StdString	other_tag;
	StdStr		want_tag;
	StdStr		want_unknown;
	const StdStr *message;
} doltgres_locale_tests_parts_t;

/* doltgres_locale_tests_parts_body reads the other name's answer where there is one and compares the two. */
static void
doltgres_locale_tests_parts_body(void *frame)
{
	doltgres_locale_tests_parts_t *f = frame;
	StdStr		tag = {f->tag.ptr, f->tag.len};
	StdOptionRef unknown = {f->unknown.ptr != NULL, {&f->unknown, std_str_Debug_fmt}};
	StdOptionRef want_unknown;
	StdTupleRef left;
	StdTupleRef right;
	doltgres_locale_tests_message_t message;

	if (f->other.ptr != NULL)
	{
		doltgres_locale_tests_parts(f->other, &f->other_tag, &f->want_unknown);
		f->other_live = true;
		f->want_tag.ptr = f->other_tag.ptr;
		f->want_tag.len = f->other_tag.len;
	}
	if (doltgres_locale_eq(tag, f->want_tag) && doltgres_locale_tests_option_eq(f->unknown, f->want_unknown))
		return;
	want_unknown.has = f->want_unknown.ptr != NULL;
	want_unknown.some = STD_ITEM(&f->want_unknown, std_str_Debug_fmt);
	left = STD_TUPLE(2, STD_ITEM(&f->tag, std_String_Debug_fmt), STD_ITEM(&unknown, std_option_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&f->want_tag, std_str_Debug_fmt), STD_ITEM(&want_unknown, std_option_Debug_fmt));
	std_assert_failed(RUST_FILE, f->line, f->col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
					  doltgres_locale_tests_message(&message, f->message));
}

/* doltgres_locale_tests_parts_pad frees the answers an unwound comparison held. */
static void
doltgres_locale_tests_parts_pad(void *frame)
{
	doltgres_locale_tests_parts_t *f = frame;

	if (f->other_live)
		std_String_drop(&f->other_tag);
	std_String_drop(&f->tag);
}

/*
 * doltgres_locale_tests_assert_parts is assert_eq! at a line and column of
 * parts' answer for a name and either parts' answer for another name or a
 * tag and an unknown part, with a message of a str or none.
 */
static void
doltgres_locale_tests_assert_parts(uint32 line, uint32 col, StdStr name, StdStr other, StdStr want_tag,
								   StdStr want_unknown, const StdStr *message)
{
	doltgres_locale_tests_parts_t frame = {
		.line = line,
		.col = col,
		.other = other,
		.want_tag = want_tag,
		.want_unknown = want_unknown,
		.message = message,
	};

	doltgres_locale_tests_parts(name, &frame.tag, &frame.unknown);
	std_landing(&frame, doltgres_locale_tests_parts_body, doltgres_locale_tests_parts_pad);
	doltgres_locale_tests_parts_pad(&frame);
}

/* DOLTGRES_LOCALE_TESTS_PARTS is assert_eq!(parts(name), (tag.to_owned(), unknown)) at a line, NULL for None. */
#define DOLTGRES_LOCALE_TESTS_PARTS(line, name, tag, unknown) \
	doltgres_locale_tests_assert_parts((line), 9, STD_STR(name), DOLTGRES_LOCALE_NONE, STD_STR(tag), \
									   doltgres_locale_tests_c(unknown), NULL)

/*
 * doltgres_locale_tests_rules_t is an assert_eq! of a name's rules: its
 * line and column, the rules, the rules they should be, and the str its
 * message shows, NULL for none.
 */
typedef struct doltgres_locale_tests_rules_t
{
	uint32		line;
	uint32		col;
	DoltgresLocaleRules rules;
	DoltgresLocaleRules want;
	const StdStr *message;
} doltgres_locale_tests_rules_t;

/* doltgres_locale_tests_rules_body compares the rules. */
static void
doltgres_locale_tests_rules_body(void *frame)
{
	doltgres_locale_tests_rules_t *f = frame;
	doltgres_locale_tests_message_t message;

	if (!doltgres_locale_Rules_PartialEq_eq(&f->rules, &f->want))
		std_assert_failed(RUST_FILE, f->line, f->col, "==", &f->rules, doltgres_locale_Rules_Debug_fmt, &f->want,
						  doltgres_locale_Rules_Debug_fmt, doltgres_locale_tests_message(&message, f->message));
}

/* doltgres_locale_tests_rules_pad drops the rules an unwound comparison held. */
static void
doltgres_locale_tests_rules_pad(void *frame)
{
	doltgres_locale_tests_rules_t *f = frame;

	doltgres_locale_Rules_drop(&f->rules);
}

/*
 * doltgres_locale_tests_assert_named is assert_eq!(named(name), want) at a
 * line and column, for rules of no tag, with a message of a str or none.
 */
static void
doltgres_locale_tests_assert_named(uint32 line, uint32 col, StdStr name, DoltgresLocaleRulesTag want,
								   const StdStr *message)
{
	doltgres_locale_tests_rules_t frame = {.line = line, .col = col, .message = message};

	frame.want.tag = want;
	frame.rules = doltgres_locale_named(name);
	std_landing(&frame, doltgres_locale_tests_rules_body, doltgres_locale_tests_rules_pad);
	doltgres_locale_tests_rules_pad(&frame);
}

/*
 * doltgres_locale_tests_asked_t is an assert_eq! of asked's answer: its
 * line, the answer, the name it should be, the name whose rules it should
 * have or None for Rules::Unknown, and those rules once held.
 */
typedef struct doltgres_locale_tests_asked_t
{
	uint32		line;
	StdStr		name;
	DoltgresLocaleRules rules;
	StdStr		want_name;
	StdStr		want_named;
	bool		want_live;
	DoltgresLocaleRules want;
} doltgres_locale_tests_asked_t;

/* doltgres_locale_tests_asked_body reads the rules the answer should have and compares the two. */
static void
doltgres_locale_tests_asked_body(void *frame)
{
	doltgres_locale_tests_asked_t *f = frame;
	StdTupleRef left;
	StdTupleRef right;

	if (f->want_named.ptr != NULL)
	{
		f->want = doltgres_locale_named(f->want_named);
		f->want_live = true;
	}
	else
		f->want.tag = DOLTGRES_LOCALE_RULES_UNKNOWN;
	if (doltgres_locale_eq(f->name, f->want_name) && doltgres_locale_Rules_PartialEq_eq(&f->rules, &f->want))
		return;
	left = STD_TUPLE(2, STD_ITEM(&f->name, std_str_Debug_fmt), STD_ITEM(&f->rules, doltgres_locale_Rules_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&f->want_name, std_str_Debug_fmt),
					  STD_ITEM(&f->want, doltgres_locale_Rules_Debug_fmt));
	std_assert_failed(RUST_FILE, f->line, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
}

/* doltgres_locale_tests_asked_pad drops the rules an unwound comparison held. */
static void
doltgres_locale_tests_asked_pad(void *frame)
{
	doltgres_locale_tests_asked_t *f = frame;

	if (f->want_live)
		doltgres_locale_Rules_drop(&f->want);
	doltgres_locale_Rules_drop(&f->rules);
}

/*
 * doltgres_locale_tests_assert_asked is assert_eq!(asked(collate, ctype,
 * second), (name, named(want))) at a line, Rules::Unknown for a NULL want.
 */
static void
doltgres_locale_tests_assert_asked(uint32 line, const char *collate, const char *ctype, bool second,
								   const char *name, const char *want)
{
	doltgres_locale_tests_asked_t frame = {
		.line = line,
		.want_name = doltgres_locale_tests_c(name),
		.want_named = doltgres_locale_tests_c(want),
	};

	doltgres_locale_asked(doltgres_locale_tests_c(collate), doltgres_locale_tests_c(ctype), second, &frame.name,
						  &frame.rules);
	std_landing(&frame, doltgres_locale_tests_asked_body, doltgres_locale_tests_asked_pad);
	doltgres_locale_tests_asked_pad(&frame);
}

/* doltgres_locale_tests_collator returns Opened::Collator of its fields. */
static DoltgresLocaleOpened
doltgres_locale_tests_collator(bool collate_c, DoltgresLocaleClasses classes, bool second)
{
	DoltgresLocaleOpened opened = {.tag = DOLTGRES_LOCALE_OPENED_COLLATOR};

	opened.u.collator.collate_c = collate_c;
	opened.u.collator.classes = classes;
	opened.u.collator.second = second;
	return opened;
}

/*
 * doltgres_locale_tests_assert_opened is assert_eq!(opened(collate, ctype),
 * want) at a line and column, with the arguments of its message or NULL.
 */
static void
doltgres_locale_tests_assert_opened(uint32 line, uint32 col, const char *collate, const char *ctype,
									DoltgresLocaleOpened want, const StdFmtArguments *message)
{
	DoltgresLocaleOpened opened = doltgres_locale_opened(doltgres_locale_tests_c(collate),
														 doltgres_locale_tests_c(ctype));

	if (!doltgres_locale_Opened_PartialEq_eq(&opened, &want))
		std_assert_failed(RUST_FILE, line, col, "==", &opened, doltgres_locale_Opened_Debug_fmt, &want,
						  doltgres_locale_Opened_Debug_fmt, message);
}

/*
 * doltgres_locale_tests_assert_pair is assert_eq!(opened(collate, ctype),
 * want, "{collate} {ctype}") at a line and column.
 */
static void
doltgres_locale_tests_assert_pair(uint32 line, uint32 col, const char *collate, const char *ctype,
								  DoltgresLocaleOpened want)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
	StdStr		collate_str = doltgres_locale_tests_c(collate);
	StdStr		ctype_str = doltgres_locale_tests_c(ctype);
	StdFmtArg	args[] = {{&collate_str, std_str_Display_fmt}, {&ctype_str, std_str_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};

	doltgres_locale_tests_assert_opened(line, col, collate, ctype, want, &message);
}

/*
 * doltgres_locale_tests_index returns `row[index]` of a row's line, its
 * fields split at tabs, with Rust's panic at a line and column for a field
 * past the row's last.
 */
static StdStr
doltgres_locale_tests_index(StdStr row, size_t index, uint32 line, uint32 col)
{
	doltgres_locale_split_t fields = doltgres_locale_split(row, '\t');
	StdStr		field;
	StdStr		found = DOLTGRES_LOCALE_NONE;
	size_t		len = 0;

	while (doltgres_locale_split_next(&fields, &field))
		if (len++ == index)
			found = field;
	std_index_check(index, len, RUST_FILE, line, col);
	return found;
}

/* doltgres_locale_tests_lines collects the lines of a table's rows, which stand for the rows' fields. */
static void
doltgres_locale_tests_lines(StdStr table, StdVec *lines)
{
	doltgres_locale_rows_t rows = doltgres_locale_rows(table);
	doltgres_locale_split_t row;

	while (doltgres_locale_rows_next(&rows, &row))
		std_vec_push_Checked(lines, &row.rest, DOLTGRES_LOCALE_TESTS_LINE);
}

/*
 * test_doltgres_locale_tests_the_c_locales_sort_by_bytes checks that `C`
 * and `POSIX` sort by bytes with C's classes, and so does `C.` with an
 * encoding, but for UTF-8's.
 */
void
test_doltgres_locale_tests_the_c_locales_sort_by_bytes(void)
{
	doltgres_locale_tests_assert_named(251, 9, STD_STR("C"), DOLTGRES_LOCALE_RULES_C, NULL);
	doltgres_locale_tests_assert_named(252, 9, STD_STR("POSIX"), DOLTGRES_LOCALE_RULES_C, NULL);
	doltgres_locale_tests_assert_named(253, 9, STD_STR("C.UTF-8"), DOLTGRES_LOCALE_RULES_C_UTF8, NULL);
	doltgres_locale_tests_assert_named(254, 9, STD_STR("C.utf8"), DOLTGRES_LOCALE_RULES_C_UTF8, NULL);
	doltgres_locale_tests_assert_named(255, 9, STD_STR("C.ISO-8859-1"), DOLTGRES_LOCALE_RULES_C, NULL);
}

/*
 * test_doltgres_locale_tests_a_posix_name_is_its_language_and_territory
 * checks that a POSIX name is its language and territory, whatever encoding
 * it states.
 */
void
test_doltgres_locale_tests_a_posix_name_is_its_language_and_territory(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(261, "de_DE", "de-DE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(262, "de_DE.UTF-8", "de-DE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(263, "de", "de", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(264, "ast_ES.utf8", "ast-ES", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(265, "en_US.ISO8859-1", "en-US", NULL);
}

/*
 * test_doltgres_locale_tests_a_modifier_is_the_subtag_it_names checks that
 * a modifier is the script or variant the table gives it, `@euro` is
 * nothing, and any other is said to be unknown beside the tag without it.
 */
void
test_doltgres_locale_tests_a_modifier_is_the_subtag_it_names(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(272, "sr_RS@latin", "sr-Latn-RS", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(273, "sr_RS", "sr-RS", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(274, "uz_UZ.UTF-8@cyrillic", "uz-Cyrl-UZ", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(275, "ca_ES@valencia", "ca-ES-valencia", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(276, "de_DE.ISO-8859-15@euro", "de-DE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(277, "aa_ER@saaho", "aa-ER", "saaho");
}

/*
 * test_doltgres_locale_tests_a_posix_name_may_state_its_script checks that
 * a POSIX name that states its script is the tag of its language, script,
 * and territory, and a modifier that names a script beside it is unknown.
 */
void
test_doltgres_locale_tests_a_posix_name_may_state_its_script(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(284, "sr_Cyrl_RS.UTF-8", "sr-Cyrl-RS", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(285, "zh_Hans_CN", "zh-Hans-CN", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(286, "sr_Cyrl_RS@latin", "sr-Cyrl-RS", "latin");
	doltgres_locale_tests_assert_named(287, 9, STD_STR("sr_cyrl_RS"), DOLTGRES_LOCALE_RULES_UNKNOWN, NULL);
	doltgres_locale_tests_assert_named(288, 9, STD_STR("sr_Cyrl_RS_x"), DOLTGRES_LOCALE_RULES_UNKNOWN, NULL);
}

/*
 * test_doltgres_locale_tests_a_language_tag_is_itself checks that a
 * language tag is itself, and names the same locale as the POSIX name of
 * its language and territory.
 */
void
test_doltgres_locale_tests_a_language_tag_is_itself(void)
{
	doltgres_locale_tests_assert_parts(294, 9, STD_STR("de-DE"), STD_STR("de_DE"), DOLTGRES_LOCALE_NONE,
									   DOLTGRES_LOCALE_NONE, NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(295, "en-US", "en-US", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(296, "sr-Latn-RS", "sr-Latn-RS", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(297, "ca-ES-valencia", "ca-ES-valencia", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(298, "es-419", "es-419", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(299, "en-US.utf8", "en-US", NULL);
}

/*
 * test_doltgres_locale_tests_a_sort_name_is_its_rows_tag_or_unknown checks
 * that Windows' sort name after a tag is the tag the sorts' table has for
 * the two, whatever their letters' case, and is otherwise said to be
 * unknown beside the tag.
 */
void
test_doltgres_locale_tests_a_sort_name_is_its_rows_tag_or_unknown(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(306, "de-DE_phoneb", "de-DE-u-co-phonebk", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(307, "es-ES_tradnl", "es-ES-u-co-trad", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(308, "zh-CN_stroke", "zh-CN-u-co-stroke", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(309, "DE-de_PHONEB.1252", "de-DE-u-co-phonebk", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(310, "zh-SG_stroke", "zh-SG", "stroke");
	DOLTGRES_LOCALE_TESTS_PARTS(311, "zh-CN_phoneb", "zh-CN", "phoneb");
	DOLTGRES_LOCALE_TESTS_PARTS(312, "zh-TW_pronun", "zh-TW", "pronun");
	DOLTGRES_LOCALE_TESTS_PARTS(313, "ja-JP_radstr", "ja-JP", "radstr");
	DOLTGRES_LOCALE_TESTS_PARTS(314, "hu-HU_technl", "hu-HU", "technl");
	DOLTGRES_LOCALE_TESTS_PARTS(315, "ka-GE_modern", "ka-GE", "modern");
	DOLTGRES_LOCALE_TESTS_PARTS(316, "x-IV_mathan", "x-IV", "mathan");
}

/*
 * doltgres_locale_tests_sorts_t is every_sort_is_one_icu_maps' frame: ICU's
 * table with its whitespace taken out, and the sorts' table's lines.
 */
typedef struct doltgres_locale_tests_sorts_t
{
	StdString	icu;
	StdVec		sorts;
} doltgres_locale_tests_sorts_t;

/* doltgres_locale_tests_every_sort_is_one_icu_maps_body checks each row of the sorts' table against ICU's. */
static void
doltgres_locale_tests_every_sort_is_one_icu_maps_body(void *frame)
{
	doltgres_locale_tests_sorts_t *f = frame;
	StdStr		locmap = DOLTGRES_LOCALE_TESTS_LOCMAP;
	StdChars	chars = std_str_chars(locmap.ptr, locmap.len);
	size_t		start = 0;
	size_t		at = 0;
	size_t		three = 3;
	uint32		c;

	for (;;)
	{
		bool		more = std_str_Chars_next_Checked(&chars, &c);
		size_t		next = locmap.len - (size_t) (chars.iter.end - chars.iter.ptr);

		if (!more || std_char_is_whitespace(c))
		{
			std_string_push_str_Checked(&f->icu, locmap.ptr + start, at - start);
			start = next;
		}
		if (!more)
			break;
		at = next;
	}
	doltgres_locale_tests_lines(DOLTGRES_LOCALE_SORTS, &f->sorts);
	STD_ASSERT_EQ(325, 9, f->sorts.len == three, &f->sorts.len, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	for (size_t i = 0; i < f->sorts.len; i++)
	{
		StdStr		row = STD_VEC_AT(&f->sorts, StdStr, i);
		StdStr		id = doltgres_locale_tests_index(row, 1, 327, 60);
		StdStr		locale = doltgres_locale_tests_index(row, 2, 327, 68);
		StdString	wanted;
		bool		found;
		StdStr		sort;
		StdStr		tag;

		std_vec_with_capacity_Checked(&wanted, id.len + locale.len + 5, 1, 1);
		std_string_push_str_Checked(&wanted, "{", 1);
		std_string_push_str_Checked(&wanted, id.ptr, id.len);
		std_string_push_str_Checked(&wanted, ",\"", 2);
		std_string_push_str_Checked(&wanted, locale.ptr, locale.len);
		std_string_push_str_Checked(&wanted, "\"}", 2);
		found = std_str_contains_str_Checked(f->icu.ptr, f->icu.len, wanted.ptr, wanted.len);
		std_String_drop(&wanted);
		if (!found)
			doltgres_locale_tests_panic(327, 13, doltgres_locale_tests_index(row, 0, 327, 84));
		tag = doltgres_locale_cut(doltgres_locale_tests_index(row, 0, 328, 28), '_', &sort);
		if (sort.ptr == NULL)
			STD_OPTION_UNWRAP_FAILED(328, 51);
		locale = doltgres_locale_tests_index(row, 3, 329, 21);
		std_vec_with_capacity_Checked(&wanted, tag.len + 6, 1, 1);
		std_string_push_str_Checked(&wanted, tag.ptr, tag.len);
		std_string_push_str_Checked(&wanted, "-u-co-", 6);
		found = locale.len >= wanted.len && memcmp(locale.ptr, wanted.ptr, wanted.len) == 0;
		std_String_drop(&wanted);
		if (!found)
			doltgres_locale_tests_panic(329, 13, doltgres_locale_tests_index(row, 0, 329, 72));
	}
}

/* doltgres_locale_tests_every_sort_is_one_icu_maps_pad frees what an unwound check held. */
static void
doltgres_locale_tests_every_sort_is_one_icu_maps_pad(void *frame)
{
	doltgres_locale_tests_sorts_t *f = frame;

	std_vec_free_Checked(&f->sorts, DOLTGRES_LOCALE_TESTS_LINE);
	std_String_drop(&f->icu);
}

/*
 * test_doltgres_locale_tests_every_sort_is_one_icu_maps checks that each
 * row of the sorts' table is ICU's own: the vendored ICU maps the row's
 * Windows locale id to the row's locale, and the row's tag is its name's tag
 * with the collation after it.
 */
void
test_doltgres_locale_tests_every_sort_is_one_icu_maps(void)
{
	doltgres_locale_tests_sorts_t frame = {.icu = STD_VEC_NEW, .sorts = STD_VEC_NEW};

	std_landing(&frame, doltgres_locale_tests_every_sort_is_one_icu_maps_body,
				doltgres_locale_tests_every_sort_is_one_icu_maps_pad);
	doltgres_locale_tests_every_sort_is_one_icu_maps_pad(&frame);
}

/*
 * test_doltgres_locale_tests_a_windows_name_is_its_rows_tag checks that one
 * of Windows' traditional names is its row's tag, whatever its letters' case
 * and its code page.
 */
void
test_doltgres_locale_tests_a_windows_name_is_its_rows_tag(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(336, "English_United States.1252", "en-US", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(337, "english_united states.1252", "en-US", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(338, "Swedish_Sweden.1252", "sv-SE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(339, "Turkish_T\xc3\xbcrkiye.1254", "tr-TR", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(340, "Serbian (Latin)_Serbia.1250", "sr-Latn-RS", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(341, "German_Germany.utf8", "de-DE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(342, "Punjabi", "pa-IN", NULL);
}

/*
 * test_doltgres_locale_tests_a_glibc_alias_is_its_locales_tag checks that
 * glibc's aliases are their locales' tags, whatever the case of their
 * letters, and where Windows' table or a POSIX name's parse gives another
 * tag, glibc's wins.
 */
void
test_doltgres_locale_tests_a_glibc_alias_is_its_locales_tag(void)
{
	static const char *const aliases[][2] = {
		{"german", "de-DE"},
		{"GERMAN", "de-DE"},
		{"bokmal", "nb-NO"},
		{"japanese.sjis", "ja-JP"},
		{"ja_JP.ujis", "ja-JP"},
		{"no_NO", "nb-NO"},
		{"no_NO.ISO-8859-1", "nb-NO"},
		{"portuguese", "pt-PT"},
		{"Portuguese", "pt-PT"},
		{"thai", "th-TH"},
	};

	for (size_t i = 0; i < lengthof(aliases); i++)
	{
		StdStr		name = doltgres_locale_tests_c(aliases[i][0]);

		doltgres_locale_tests_assert_parts(361, 13, name, DOLTGRES_LOCALE_NONE, doltgres_locale_tests_c(aliases[i][1]),
										   DOLTGRES_LOCALE_NONE, &name);
	}
	DOLTGRES_LOCALE_TESTS_PARTS(363, "no_NO.UTF-8", "no-NO", NULL);
}

/*
 * test_doltgres_locale_tests_the_names_postgres_maps_are_rows checks that
 * the names Postgres' own `win32setlocale.c` maps are rows, a country's dots
 * included.
 */
void
test_doltgres_locale_tests_the_names_postgres_maps_are_rows(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(369, "Chinese (Traditional)_Hong Kong S.A.R..950", "zh-HK", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(370, "Chinese (Traditional)_Hong Kong S.A.R.", "zh-HK", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(371, "Chinese (Traditional)_Macau S.A.R..950", "zh-MO", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(372, "Arabic_U.A.E..1256", "ar-AE", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(373, "Norwegian_Norway.1252", "nb-NO", NULL);
}

/*
 * test_doltgres_locale_tests_the_names_of_earlier_windows_releases_are_rows
 * checks that the spellings of earlier Windows releases, which Microsoft's
 * published list holds, are rows.
 */
void
test_doltgres_locale_tests_the_names_of_earlier_windows_releases_are_rows(void)
{
	DOLTGRES_LOCALE_TESTS_PARTS(379, "Turkish_Turkey.1254", "tr-TR", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(380, "Czech_Czech Republic.1250", "cs-CZ", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(381, "Chinese (Simplified)_People's Republic of China.936", "zh-CN", NULL);
	DOLTGRES_LOCALE_TESTS_PARTS(382, "Lao_Lao P.D.R.", "lo-LA", NULL);
}

/*
 * test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name_closure_0
 * answers the part a name's tag holds nothing for, or false for none.
 */
static bool
test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name_closure_0(StdStr name,
																				  DoltgresLocalePart *result)
{
	DoltgresLocaleRules rules = doltgres_locale_named(name);
	bool		has;

	if (rules.tag != DOLTGRES_LOCALE_RULES_TAG)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&rules, doltgres_locale_Rules_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 390, 22, &message);
	}
	has = rules.u.tag.has_unknown;
	if (has)
		*result = rules.u.tag.unknown;
	doltgres_locale_Rules_drop(&rules);
	return has;
}

/* doltgres_locale_tests_assert_unknown is assert_eq! at a line of the part a name's tag holds nothing for. */
static void
doltgres_locale_tests_assert_unknown(uint32 line, const char *name, bool want_has, DoltgresLocalePart want)
{
	StdStr		named = doltgres_locale_tests_c(name);
	DoltgresLocalePart part = {0};
	bool		has = test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name_closure_0(named, &part);
	StdOptionRef left = {has, {&part, doltgres_locale_Part_Debug_fmt}};
	StdOptionRef right = {want_has, {&want, doltgres_locale_Part_Debug_fmt}};

	STD_ASSERT_EQ(line, 9, has == want_has && (!has || doltgres_locale_Part_PartialEq_eq(&part, &want)), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
}

/*
 * test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name
 * checks that what a tag holds nothing for is a POSIX name's modifier or the
 * sort name after a tag, each said as what it is.
 */
void
test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name(void)
{
	DoltgresLocalePart modifier = {.tag = DOLTGRES_LOCALE_PART_MODIFIER, .u.modifier = STD_STR("saaho")};
	DoltgresLocalePart sort = {.tag = DOLTGRES_LOCALE_PART_SORT, .u.sort = STD_STR("technl")};
	DoltgresLocalePart none = {0};

	doltgres_locale_tests_assert_unknown(392, "aa_ER@saaho", true, modifier);
	doltgres_locale_tests_assert_unknown(393, "hu-HU_technl", true, sort);
	doltgres_locale_tests_assert_unknown(394, "de_DE@euro", false, none);
}

/*
 * test_doltgres_locale_tests_the_c_family_is_postgres_own checks that two
 * names of `C` or `POSIX` are Postgres' own to build, and every other pair
 * of the `C` family the builtin constructor's.
 */
void
test_doltgres_locale_tests_the_c_family_is_postgres_own(void)
{
	static const char *const stock[][2] = {{"C", "C"}, {"POSIX", "C"}, {"C", "POSIX"}, {"POSIX", "POSIX"}};
	static const char *const builtin[][2] = {
		{"C", "C.UTF-8"}, {"C.UTF-8", "C.utf8"}, {"C.UTF-8", "C"}, {"POSIX", "C.utf8"}, {"C.ISO-8859-1", "C"},
	};
	DoltgresLocaleOpened want = {.tag = DOLTGRES_LOCALE_OPENED_STOCK};

	for (size_t i = 0; i < lengthof(stock); i++)
		doltgres_locale_tests_assert_pair(402, 13, stock[i][0], stock[i][1], want);
	want.tag = DOLTGRES_LOCALE_OPENED_BUILTIN;
	for (size_t i = 0; i < lengthof(builtin); i++)
		doltgres_locale_tests_assert_pair(407, 13, builtin[i][0], builtin[i][1], want);
}

/*
 * test_doltgres_locale_tests_a_c_utf8_ctype_takes_the_builtin_classes
 * checks that a named collate name with a `C.UTF-8` ctype is ordered by the
 * collator and classified as the builtin provider's `C.UTF-8`, and one with
 * a `C.` ctype of another encoding as C.
 */
void
test_doltgres_locale_tests_a_c_utf8_ctype_takes_the_builtin_classes(void)
{
	static const char *const ctypes[] = {"C.UTF-8", "C.utf8"};

	for (size_t i = 0; i < lengthof(ctypes); i++)
	{
		DoltgresLocaleOpened pair = doltgres_locale_tests_collator(false, DOLTGRES_LOCALE_CLASSES_C_UTF8, false);
		StdStr		ctype = doltgres_locale_tests_c(ctypes[i]);
		doltgres_locale_tests_message_t message;

		doltgres_locale_tests_assert_opened(417, 13, "en_US.UTF-8", ctypes[i], pair,
											doltgres_locale_tests_message(&message, &ctype));
	}
	doltgres_locale_tests_assert_opened(420, 9, "en_US.UTF-8", "C.ISO-8859-1",
										doltgres_locale_tests_collator(false, DOLTGRES_LOCALE_CLASSES_C, false), NULL);
	doltgres_locale_tests_assert_asked(421, "en_US.UTF-8", "C.UTF-8", false, "en_US.UTF-8", "en_US.UTF-8");
}

/*
 * test_doltgres_locale_tests_a_named_half_is_the_collators checks that one
 * name is the collator's and both halves are built from it, a half whose
 * name asks for byte order or C's classes is marked, and the name the
 * constructor is built from is the one that isn't of the `C` family.
 */
void
test_doltgres_locale_tests_a_named_half_is_the_collators(void)
{
	DoltgresLocaleOpened whole = doltgres_locale_tests_collator(false, DOLTGRES_LOCALE_CLASSES_COLLATOR, false);
	DoltgresLocaleOpened ordered = doltgres_locale_tests_collator(true, DOLTGRES_LOCALE_CLASSES_COLLATOR, false);

	doltgres_locale_tests_assert_opened(429, 9, "sv_SE.UTF-8", "sv_SE.UTF-8", whole, NULL);
	doltgres_locale_tests_assert_opened(430, 9, "Swedish_Sweden.1252", "sv-SE", whole, NULL);
	doltgres_locale_tests_assert_opened(431, 9, "Klingon_Qo'noS.1252", "Klingon_Qo'noS.1252", whole, NULL);
	doltgres_locale_tests_assert_opened(433, 9, "C", "en_US.UTF-8", ordered, NULL);
	doltgres_locale_tests_assert_opened(434, 9, "C.UTF-8", "tr_TR", ordered, NULL);
	doltgres_locale_tests_assert_opened(435, 9, "de_DE", "POSIX",
										doltgres_locale_tests_collator(false, DOLTGRES_LOCALE_CLASSES_C, false), NULL);
	doltgres_locale_tests_assert_asked(436, "sv_SE.UTF-8", "sv-SE", false, "sv_SE.UTF-8", "sv_SE.UTF-8");
	doltgres_locale_tests_assert_asked(437, "C", "en_US.UTF-8", false, "en_US.UTF-8", "en_US.UTF-8");
	doltgres_locale_tests_assert_asked(438, "de_DE", "POSIX", false, "de_DE", "de_DE");
	doltgres_locale_tests_assert_asked(439, "Klingon_Qo'noS.1252", "C", false, "Klingon_Qo'noS.1252", NULL);
}

/*
 * test_doltgres_locale_tests_two_tags_are_two_asks checks that two names of
 * different tags make the constructor build twice, the second time from the
 * ctype name.
 */
void
test_doltgres_locale_tests_two_tags_are_two_asks(void)
{
	DoltgresLocaleOpened twice = doltgres_locale_tests_collator(false, DOLTGRES_LOCALE_CLASSES_COLLATOR, true);

	doltgres_locale_tests_assert_opened(446, 9, "de_DE", "tr_TR", twice, NULL);
	doltgres_locale_tests_assert_opened(447, 9, "de_DE", "nowhere", twice, NULL);
	doltgres_locale_tests_assert_asked(448, "de_DE", "tr_TR", false, "de_DE", "de_DE");
	doltgres_locale_tests_assert_asked(449, "de_DE", "tr_TR", true, "tr_TR", "tr_TR");
}

/*
 * test_doltgres_locale_tests_a_name_of_no_form_is_unknown checks that a
 * name of no form, or of Windows' form with no row, is unknown.
 */
void
test_doltgres_locale_tests_a_name_of_no_form_is_unknown(void)
{
	static const char *const names[] = {
		"", "Klingon_Qo'noS.1252", "POSIX.UTF-8", "de_DE.", "de_DE@", "de_de", "DE_DE", "en-US_", "c",
	};

	for (size_t i = 0; i < lengthof(names); i++)
	{
		StdStr		name = doltgres_locale_tests_c(names[i]);

		doltgres_locale_tests_assert_named(456, 13, name, DOLTGRES_LOCALE_RULES_UNKNOWN, &name);
	}
}

/* doltgres_locale_tests_every_row_is_found_under_its_name_body checks each row of glibc's table and of Windows'. */
static void
doltgres_locale_tests_every_row_is_found_under_its_name_body(void *frame)
{
	StdVec	   *names = frame;
	StdStr		tables[] = {DOLTGRES_LOCALE_GLIBC, DOLTGRES_LOCALE_WINDOWS};
	size_t		leasts[] = {45, 900};

	for (size_t t = 0; t < lengthof(tables); t++)
	{
		size_t		least = leasts[t];

		doltgres_locale_tests_lines(tables[t], names);
		STD_ASSERT(467, 13, names->len >= least, "names.len() >= least");
		for (size_t at = 0; at < names->len; at++)
		{
			StdStr		row = STD_VEC_AT(names, StdStr, at);
			StdStr		name = doltgres_locale_tests_index(row, 0, 469, 40);
			StdStr		tag;
			bool		aliased = doltgres_locale_field(DOLTGRES_LOCALE_GLIBC, name, 0, &tag);
			StdStr		own = doltgres_locale_tests_index(row, 1, 469, 61);

			if (!aliased)
				tag = own;
			doltgres_locale_tests_assert_parts(470, 17, name, DOLTGRES_LOCALE_NONE, tag, DOLTGRES_LOCALE_NONE,
											   &name);
			for (size_t earlier = 0; earlier < at; earlier++)
			{
				StdStr		first = doltgres_locale_tests_index(STD_VEC_AT(names, StdStr, earlier), 0, 471, 59);

				if (doltgres_locale_eq_ignore_ascii_case(first, name))
					doltgres_locale_tests_panic(471, 17, name);
			}
		}
		std_vec_free_Checked(names, DOLTGRES_LOCALE_TESTS_LINE);
		*names = (StdVec) STD_VEC_NEW;
	}
}

/* doltgres_locale_tests_every_row_is_found_under_its_name_pad frees the lines an unwound check held. */
static void
doltgres_locale_tests_every_row_is_found_under_its_name_pad(void *frame)
{
	std_vec_free_Checked(frame, DOLTGRES_LOCALE_TESTS_LINE);
}

/*
 * test_doltgres_locale_tests_every_row_is_found_under_its_name checks that
 * every row of glibc's table and of Windows' is found under its own name,
 * but for a Windows name glibc's table holds with another tag, which is
 * glibc's, and that no two rows' names in a table differ by their letters'
 * case alone.
 */
void
test_doltgres_locale_tests_every_row_is_found_under_its_name(void)
{
	StdVec		names = STD_VEC_NEW;

	std_landing(&names, doltgres_locale_tests_every_row_is_found_under_its_name_body,
				doltgres_locale_tests_every_row_is_found_under_its_name_pad);
	doltgres_locale_tests_every_row_is_found_under_its_name_pad(&names);
}

#endif							/* DOLTGRES_TEST_DOLTGRES */
