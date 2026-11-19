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
 *
 * Portions Copyright (c) 2002-2026, PostgreSQL Global Development Group
 */

/*
 * The builtin provider's C.UTF-8 character classes and case mapping (design
 * item 12), for a libc locale whose collate name is a named locale's and whose
 * ctype name is C.UTF-8: pg_locale_builtin.c's ctype methods written again, in
 * its order and with its names, reading C.UTF-8's simple case mapping from
 * casemap_full in place of the locale's builtin fields, which a locale ICU's
 * constructor built doesn't have.
 */

#include "postgres.h"

#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "doltgres/locale_ctype.h"
#include "mb/pg_wchar.h"

/* casemap_full is C.UTF-8's case mapping: simple, not full. */
static const bool casemap_full = false;

/* WordBoundaryState is initcap_wbnext's place in a string. */
struct WordBoundaryState
{
	const char *str;
	size_t		len;
	size_t		offset;
	bool		posix;
	bool		init;
	bool		prev_alnum;
};

/* to_char32 returns a UTF-8 database's character as its code point. */
static inline char32_t
to_char32(pg_wchar wc)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);
	return (char32_t) wc;
}

/* to_pg_wchar returns a code point as a UTF-8 database's character. */
static inline pg_wchar
to_pg_wchar(char32_t c32)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);
	return (pg_wchar) c32;
}

/*
 * initcap_wbnext returns the next word boundary, drawn each time the result
 * of pg_u_isalnum() changes.
 */
static size_t
initcap_wbnext(void *state)
{
	struct WordBoundaryState *wbstate = (struct WordBoundaryState *) state;

	while (wbstate->offset < wbstate->len)
	{
		int			ulen = pg_utf_mblen((const unsigned char *) wbstate->str +
										wbstate->offset);
		char32_t	u;
		bool		curr_alnum;
		size_t		prev_offset = wbstate->offset;

		/* invalid UTF8 */
		if (wbstate->offset + ulen > wbstate->len)
		{
			wbstate->init = true;
			wbstate->offset = wbstate->len;
			return prev_offset;
		}

		u = utf8_to_unicode((const unsigned char *) wbstate->str +
							wbstate->offset);
		curr_alnum = pg_u_isalnum(u, wbstate->posix);

		if (!wbstate->init || curr_alnum != wbstate->prev_alnum)
		{
			wbstate->init = true;
			wbstate->offset += ulen;
			wbstate->prev_alnum = curr_alnum;
			return prev_offset;
		}

		wbstate->offset += ulen;
	}

	return wbstate->len;
}

/* strlower_builtin lowers a string's case. */
static size_t
strlower_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	return unicode_strlower(dest, destsize, src, srclen,
							casemap_full);
}

/* strtitle_builtin gives each word of a string its title case. */
static size_t
strtitle_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	struct WordBoundaryState wbstate = {
		.str = src,
		.len = srclen,
		.offset = 0,
		.posix = !casemap_full,
		.init = false,
		.prev_alnum = false,
	};

	return unicode_strtitle(dest, destsize, src, srclen,
							casemap_full,
							initcap_wbnext, &wbstate);
}

/* strupper_builtin raises a string's case. */
static size_t
strupper_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	return unicode_strupper(dest, destsize, src, srclen,
							casemap_full);
}

/* strfold_builtin folds a string's case. */
static size_t
strfold_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				pg_locale_t locale)
{
	return unicode_strfold(dest, destsize, src, srclen,
						   casemap_full);
}

/* wc_isdigit_builtin reports whether a character is a digit. */
static bool
wc_isdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isdigit(to_char32(wc), !casemap_full);
}

/* wc_isalpha_builtin reports whether a character is a letter. */
static bool
wc_isalpha_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalpha(to_char32(wc));
}

/* wc_isalnum_builtin reports whether a character is a letter or a digit. */
static bool
wc_isalnum_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalnum(to_char32(wc), !casemap_full);
}

/* wc_isupper_builtin reports whether a character is upper case. */
static bool
wc_isupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isupper(to_char32(wc));
}

/* wc_islower_builtin reports whether a character is lower case. */
static bool
wc_islower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_islower(to_char32(wc));
}

/* wc_isgraph_builtin reports whether a character is visible. */
static bool
wc_isgraph_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isgraph(to_char32(wc));
}

/* wc_isprint_builtin reports whether a character is printable. */
static bool
wc_isprint_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isprint(to_char32(wc));
}

/* wc_ispunct_builtin reports whether a character is punctuation. */
static bool
wc_ispunct_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_ispunct(to_char32(wc), !casemap_full);
}

/* wc_isspace_builtin reports whether a character is white space. */
static bool
wc_isspace_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isspace(to_char32(wc));
}

/* wc_isxdigit_builtin reports whether a character is a hexadecimal digit. */
static bool
wc_isxdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isxdigit(to_char32(wc), !casemap_full);
}

/* wc_iscased_builtin reports whether a character has case. */
static bool
wc_iscased_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_prop_cased(to_char32(wc));
}

/* wc_toupper_builtin returns a character's simple upper case. */
static pg_wchar
wc_toupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return to_pg_wchar(unicode_uppercase_simple(to_char32(wc)));
}

/* wc_tolower_builtin returns a character's simple lower case. */
static pg_wchar
wc_tolower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return to_pg_wchar(unicode_lowercase_simple(to_char32(wc)));
}

/* doltgres_ctype_methods_builtin is the builtin provider's C.UTF-8 methods. */
const struct ctype_methods doltgres_ctype_methods_builtin = {
	.strlower = strlower_builtin,
	.strtitle = strtitle_builtin,
	.strupper = strupper_builtin,
	.strfold = strfold_builtin,
	.wc_isdigit = wc_isdigit_builtin,
	.wc_isalpha = wc_isalpha_builtin,
	.wc_isalnum = wc_isalnum_builtin,
	.wc_isupper = wc_isupper_builtin,
	.wc_islower = wc_islower_builtin,
	.wc_isgraph = wc_isgraph_builtin,
	.wc_isprint = wc_isprint_builtin,
	.wc_ispunct = wc_ispunct_builtin,
	.wc_isspace = wc_isspace_builtin,
	.wc_isxdigit = wc_isxdigit_builtin,
	.wc_iscased = wc_iscased_builtin,
	.wc_tolower = wc_tolower_builtin,
	.wc_toupper = wc_toupper_builtin,
};
