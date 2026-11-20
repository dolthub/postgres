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
 * The locale entries (doltgres' entries.rs, its locale section): the bodies
 * that build a libc collation's locale from Postgres' ICU and builtin
 * constructors, mark a new collation's check, and give those constructors and
 * a collation's version the tags a stored name stands for, with the helpers
 * the crate's other entries call as well.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/locale.h"
#include "doltgres/host/local.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

/*
 * DOLTGRES_ENTRIES_SECOND is SECOND: whether the libc site is asking the ICU
 * constructor a second time, for a locale's ctype half, which tells
 * doltgres_icu_locale to give the ctype name's tag; its BackendLocal header
 * and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicBool process;
}			DOLTGRES_ENTRIES_SECOND = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* doltgres_entries_Second_ask is Second::ask: it marks the second ask. */
static void
doltgres_entries_Second_ask(void)
{
	HostLocalLocal local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_SECOND.local, &STD_ATOMIC_BOOL_TYPE);

	std_AtomicBool_store_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_BOOL_TYPE), true);
	host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
}

/* doltgres_entries_Second_drop is Second's drop: it clears the mark of the second ask. */
static void
doltgres_entries_Second_drop(void)
{
	HostLocalLocal asking;

	if (host_local_BackendLocal_existing(&DOLTGRES_ENTRIES_SECOND.local, &asking))
	{
		std_AtomicBool_store_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&asking, &STD_ATOMIC_BOOL_TYPE),
									 false);
		host_local_Local_drop(&asking, &STD_ATOMIC_BOOL_TYPE);
	}
}

/* doltgres_entries_second_asked returns whether the libc site is asking the ICU constructor a second time. */
static bool
doltgres_entries_second_asked(void)
{
	HostLocalLocal local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_SECOND.local, &STD_ATOMIC_BOOL_TYPE);
	bool		second;

	second = std_AtomicBool_load_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local,
																						&STD_ATOMIC_BOOL_TYPE));
	host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
	return second;
}

/*
 * DOLTGRES_ENTRIES_NEW_COLLATION is NEW_COLLATION: the OID of the new
 * collation CREATE COLLATION's check is loading the locale of, whose libc
 * names Postgres' C library builds first, or zero; its BackendLocal header
 * and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicU32 process;
}			DOLTGRES_ENTRIES_NEW_COLLATION = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* doltgres_entries_Loading_of is Loading::of: it marks the load of a collation's locale. */
static void
doltgres_entries_Loading_of(uint32 collid)
{
	HostLocalLocal local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_NEW_COLLATION.local, &STD_ATOMIC_U32_TYPE);

	std_AtomicU32_store_relaxed((StdAtomicU32 *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_U32_TYPE), collid);
	host_local_Local_drop(&local, &STD_ATOMIC_U32_TYPE);
}

/* doltgres_entries_Loading_drop is Loading's drop: it clears the mark of the load. */
static void
doltgres_entries_Loading_drop(void)
{
	HostLocalLocal loading;

	if (host_local_BackendLocal_existing(&DOLTGRES_ENTRIES_NEW_COLLATION.local, &loading))
	{
		std_AtomicU32_store_relaxed((StdAtomicU32 *) host_local_Local_Deref_deref(&loading, &STD_ATOMIC_U32_TYPE),
									0);
		host_local_Local_drop(&loading, &STD_ATOMIC_U32_TYPE);
	}
}

/* doltgres_entries_new_collation_loading returns the OID of the new collation whose locale is loading, or zero. */
static uint32
doltgres_entries_new_collation_loading(void)
{
	HostLocalLocal local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_NEW_COLLATION.local, &STD_ATOMIC_U32_TYPE);
	uint32		collid;

	collid = std_AtomicU32_load_relaxed((StdAtomicU32 *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_U32_TYPE));
	host_local_Local_drop(&local, &STD_ATOMIC_U32_TYPE);
	return collid;
}

/*
 * DOLTGRES_ENTRIES_HINT is HINT: the hint of Postgres' own warnings of an ICU
 * locale it can't validate, which the fork's warnings of a libc locale's name
 * carry as well, since the same setting silences both.
 */
static const char DOLTGRES_ENTRIES_HINT[] =
"To disable ICU locale validation, set the parameter \"icu_validation_level\" to \"disabled\".";

/* doltgres_entries_internal returns the fork's internal error with a message. */
PgPgError
doltgres_entries_internal(const char *message, size_t message_len)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, message_len);
	return pg_PgError_fork((const uint8 *) "XX000", &owned);
}

/*
 * doltgres_entries_whole sizes a buffer to the length a wrapper answered for
 * the text it wrote there, and gives the text when the buffer held it all, or
 * nothing when the wrapper is to be asked again with the buffer now that
 * long. Bytes that aren't UTF-8 are freed before the error is made, which an
 * unwind from it would free as well.
 */
static bool
doltgres_entries_whole(StdVec *bytes, size_t len, bool *has_result, StdString *result, PgPgError *error)
{
	static const char message[] = "a locale's name isn't UTF-8 after its conversion";
	bool		held = len <= bytes->len;
	uint8		zero = 0;
	StdVec		taken;
	StdUtf8Error invalid;

	std_vec_resize(bytes, len, &zero, 1, 1);
	if (!held)
	{
		*has_result = false;
		return true;
	}
	taken = *bytes;
	std_vec_new(bytes, 1, 1);
	if (!std_str_from_utf8((const uint8 *) taken.ptr, taken.len, &invalid))
	{
		std_vec_free_Checked(&taken, 1, 1);
		*error = doltgres_entries_internal(message, sizeof(message) - 1);
		return false;
	}
	*has_result = true;
	*result = taken;
	return true;
}

/*
 * doltgres_entries_named_frame_0_t is locale_name's and utf8_of's state
 * across their pad: their arguments, the buffer the wrapper writes into, and
 * where the name and the outcome go.
 */
typedef struct doltgres_entries_named_frame_0_t
{
	const PgCatalogCaps *caps;
	uint32		collid;
	bool		ctype;
	const char *encoded;
	StdVec		bytes;
	StdString  *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_named_frame_0_t;

/* doltgres_entries_named_pad_0 frees the buffer of an unwound read of a name. */
static void
doltgres_entries_named_pad_0(void *frame_)
{
	doltgres_entries_named_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* doltgres_entries_locale_name_body_0 asks for a locale's name until the buffer holds it all. */
static void
doltgres_entries_locale_name_body_0(void *frame_)
{
	doltgres_entries_named_frame_0_t *frame = frame_;
	bool		has = false;

	while (!has)
	{
		size_t		len;

		frame->ok = pg_calls_locale_name(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
										 frame->collid, frame->ctype, (uint8 *) frame->bytes.ptr, frame->bytes.len,
										 &len, frame->error) &&
			doltgres_entries_whole(&frame->bytes, len, &has, frame->result, frame->error);
		if (!frame->ok)
			return;
	}
}

/* doltgres_entries_locale_name returns a locale's stored collate or ctype name, as UTF-8. */
static bool
doltgres_entries_locale_name(const PgCatalogCaps *caps, uint32 collid, bool ctype, StdString *result,
							 PgPgError *error)
{
	doltgres_entries_named_frame_0_t frame = {.caps = caps, .collid = collid, .ctype = ctype, .result = result,
	.error = error};

	std_vec_u8_from_elem(&frame.bytes, 0, 128);
	std_landing(&frame, doltgres_entries_locale_name_body_0, doltgres_entries_named_pad_0);
	std_vec_free_Checked(&frame.bytes, 1, 1);
	return frame.ok;
}

/* doltgres_entries_utf8_of_body_0 asks for a name's UTF-8 until the buffer holds it all. */
static void
doltgres_entries_utf8_of_body_0(void *frame_)
{
	doltgres_entries_named_frame_0_t *frame = frame_;
	bool		has = false;

	while (!has)
	{
		size_t		len;

		frame->ok = pg_calls_utf8_of(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
									 frame->encoded, (uint8 *) frame->bytes.ptr, frame->bytes.len, &len, frame->error) &&
			doltgres_entries_whole(&frame->bytes, len, &has, frame->result, frame->error);
		if (!frame->ok)
			return;
	}
}

/* doltgres_entries_utf8_of returns a name in the database's encoding as UTF-8. */
static bool
doltgres_entries_utf8_of(const PgCatalogCaps *caps, const char *encoded, StdString *result, PgPgError *error)
{
	doltgres_entries_named_frame_0_t frame = {.caps = caps, .encoded = encoded, .result = result, .error = error};

	std_vec_u8_from_elem(&frame.bytes, 0, 128);
	std_landing(&frame, doltgres_entries_utf8_of_body_0, doltgres_entries_named_pad_0);
	std_vec_free_Checked(&frame.bytes, 1, 1);
	return frame.ok;
}

/*
 * doltgres_entries_c_string returns a text as the NUL-terminated string a
 * wrapper takes. A text holding a NUL is freed before the error is made,
 * which an unwind from it would free as well.
 */
bool
doltgres_entries_c_string(StdString *owned, StdCString *result, PgPgError *error)
{
	static const char message[] = "a locale's name holds a NUL";
	StdNulError nul;

	if (std_CString_new_String(owned, result, &nul))
		return true;
	std_NulError_drop(&nul);
	*error = doltgres_entries_internal(message, sizeof(message) - 1);
	return false;
}

/*
 * doltgres_entries_warn_frame_0_t is warn's state across its pad: its
 * arguments, the strings it makes, and where the outcome goes.
 */
typedef struct doltgres_entries_warn_frame_0_t
{
	const PgCatalogCaps *caps;
	StdString	message;
	bool		message_live;
	StdStr		tag;
	StdCString	detail;
	bool		detail_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_warn_frame_0_t;

/* doltgres_entries_warn_body_0 makes the warning's strings and raises it. */
static void
doltgres_entries_warn_body_0(void *frame_)
{
	doltgres_entries_warn_frame_0_t *frame = frame_;
	StdFmtPiece pieces[] = {STD_FMT_LIT("The locale is ordered as \""), STD_FMT_ARG(0), STD_FMT_LIT("\".")};
	StdFmtArg	args[] = {{&frame->tag, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	formatted;
	StdCString	message;

	std_fmt_format_Checked(&formatted, &arguments);
	if (!doltgres_entries_c_string(&formatted, &frame->detail, frame->error))
		return;
	frame->detail_live = true;
	frame->message_live = false;
	if (!doltgres_entries_c_string(&frame->message, &message, frame->error))
		return;
	frame->ok = pg_calls_warn(pg_CatalogCaps_may_raise(frame->caps), (const uint8 *) "22023", 5,
							  std_CString_as_ptr(&message), std_CString_as_ptr(&frame->detail),
							  DOLTGRES_ENTRIES_HINT, frame->error);
	std_CString_drop(&message);
}

/* doltgres_entries_warn_pad_0 drops what an unwound warning holds. */
static void
doltgres_entries_warn_pad_0(void *frame_)
{
	doltgres_entries_warn_frame_0_t *frame = frame_;

	if (frame->detail_live)
		std_CString_drop(&frame->detail);
	if (frame->message_live)
		std_String_drop(&frame->message);
}

/*
 * doltgres_entries_warn raises the fork's warning of a libc locale's name,
 * with the tag the locale is ordered as and Postgres' hint; it takes the
 * message.
 */
static bool
doltgres_entries_warn(const PgCatalogCaps *caps, StdString *message, StdStr tag, PgPgError *error)
{
	doltgres_entries_warn_frame_0_t frame = {.caps = caps, .message = *message, .message_live = true, .tag = tag,
	.error = error};

	std_landing(&frame, doltgres_entries_warn_body_0, doltgres_entries_warn_pad_0);
	if (frame.detail_live)
		std_CString_drop(&frame.detail);
	if (frame.message_live)
		std_String_drop(&frame.message);
	return frame.ok;
}

/*
 * doltgres_entries_kept returns a locale with the C library's locale of its
 * names kept in it, when one is given; it takes the locale.
 */
PgLocale
doltgres_entries_kept(PgLocale *locale, bool has_loc, PgLibcLocale loc)
{
	if (has_loc)
		return pg_calls_locale_keep_lt(locale, loc);
	return *locale;
}

/*
 * doltgres_entries_libc_locale_frame_0_t is libc_locale's state across its
 * pad: its arguments, the names it reads, and where the locale and the
 * outcome go.
 */
typedef struct doltgres_entries_libc_locale_frame_0_t
{
	const PgCatalogCaps *caps;
	uint32		collid;
	const PgContext *context;
	bool		has_loc;
	PgLibcLocale loc;
	StdString	collate;
	bool		collate_live;
	StdString	ctype;
	bool		ctype_live;
	PgLocaleOption *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_libc_locale_frame_0_t;

/* doltgres_entries_libc_locale_body_0 builds the locale from the collation's names. */
static void
doltgres_entries_libc_locale_body_0(void *frame_)
{
	doltgres_entries_libc_locale_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DoltgresLocaleOpened opened;
	PgIcuLocale built;
	PgLocale	locale;
	StdStr		collate;
	StdStr		ctype;

	if (!doltgres_entries_locale_name(frame->caps, frame->collid, false, &frame->collate, frame->error))
		return;
	frame->collate_live = true;
	if (!doltgres_entries_locale_name(frame->caps, frame->collid, true, &frame->ctype, frame->error))
		return;
	frame->ctype_live = true;
	collate.ptr = frame->collate.ptr;
	collate.len = frame->collate.len;
	ctype.ptr = frame->ctype.ptr;
	ctype.len = frame->ctype.len;
	opened = doltgres_locale_opened(collate, ctype);
	if (opened.tag == DOLTGRES_LOCALE_OPENED_STOCK ||
		(!frame->has_loc && doltgres_entries_new_collation_loading() == frame->collid))
	{
		frame->result->has = false;
		frame->ok = true;
		return;
	}
	if (opened.tag == DOLTGRES_LOCALE_OPENED_BUILTIN)
	{
		PgBuiltinLocale builtin;

		if (!pg_calls_locale_builtin(raise, catalog, frame->collid, frame->context, &builtin, frame->error))
			return;
		locale = pg_Locale_FromBuiltinLocale_from(&builtin);
		frame->result->has = true;
		frame->result->locale = doltgres_entries_kept(&locale, frame->has_loc, frame->loc);
		frame->ok = true;
		return;
	}
	if (!pg_calls_locale_icu(raise, catalog, frame->collid, frame->context, &built, frame->error))
		return;
	if (opened.u.collator.collate_c)
		built = pg_calls_locale_collate_c(&built);
	switch (opened.u.collator.classes)
	{
		case DOLTGRES_LOCALE_CLASSES_COLLATOR:
			break;
		case DOLTGRES_LOCALE_CLASSES_C_UTF8:
			if (pg_calls_database_is_utf8())
			{
				built = pg_calls_locale_ctype_c_utf8(&built);
				break;
			}
			pg_fallthrough;
		case DOLTGRES_LOCALE_CLASSES_C:
			built = pg_calls_locale_ctype_c(&built);
			break;
	}
	if (opened.u.collator.second)
	{
		PgIcuLocale spare;
		bool		spared;

		doltgres_entries_Second_ask();
		spared = pg_calls_locale_icu(raise, catalog, frame->collid, frame->context, &spare, frame->error);
		doltgres_entries_Second_drop();
		if (!spared)
			return;
		built = pg_calls_locale_take_ctype(&built, &spare);
	}
	locale = pg_Locale_FromIcuLocale_from(&built);
	frame->result->has = true;
	frame->result->locale = doltgres_entries_kept(&locale, frame->has_loc, frame->loc);
	frame->ok = true;
}

/* doltgres_entries_libc_locale_pad_0 drops the names an unwound build read. */
static void
doltgres_entries_libc_locale_pad_0(void *frame_)
{
	doltgres_entries_libc_locale_frame_0_t *frame = frame_;

	if (frame->ctype_live)
		std_String_drop(&frame->ctype);
	if (frame->collate_live)
		std_String_drop(&frame->collate);
}

/*
 * doltgres_entries_libc_locale builds a libc-named collation's locale,
 * doltgres_libc_locale's and doltgres_libc_new_locale's, keeping the C
 * library's locale of its names when one is given.
 */
static bool
doltgres_entries_libc_locale(const PgCatalogCaps *caps, uint32 collid, const PgContext *context, bool has_loc,
							 PgLibcLocale loc, PgLocaleOption *result, PgPgError *error)
{
	doltgres_entries_libc_locale_frame_0_t frame = {.caps = caps, .collid = collid, .context = context,
	.has_loc = has_loc, .loc = loc, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_libc_locale_body_0, doltgres_entries_libc_locale_pad_0);
	doltgres_entries_libc_locale_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_libc_locale builds the locale of a libc
 * collation, or of a database whose provider is libc, from Postgres' ICU
 * constructor (design item 12), so that its order and its classes are the
 * vendored collator's for the tag its stored names stand for, the same on
 * every platform. It answers nothing where both names are `C` or `POSIX`,
 * which Postgres' own libc code builds, builds every other pair of the `C`
 * family from the builtin provider's constructor, and leaves a new collation
 * CREATE COLLATION checks to Postgres' C library first.
 */
bool
doltgres_entries_doltgres_libc_locale(const PgCatalogCaps *caps, uint32 collid, const PgContext *context,
									  PgLocaleOption *result, PgPgError *error)
{
	PgLibcLocale none = {0};

	return doltgres_entries_libc_locale(caps, collid, context, false, none, result, error);
}

/*
 * doltgres_entries_doltgres_libc_new_locale builds the locale of a new
 * collation CREATE COLLATION checks, as doltgres_libc_locale does, once
 * Postgres' C library has made its names' locale, which it keeps.
 */
bool
doltgres_entries_doltgres_libc_new_locale(const PgCatalogCaps *caps, uint32 collid, const PgContext *context,
										  PgLibcLocale loc, PgLocaleOption *result, PgPgError *error)
{
	return doltgres_entries_libc_locale(caps, collid, context, true, loc, result, error);
}

/*
 * doltgres_entries_doltgres_new_collation loads a new collation's locale for
 * CREATE COLLATION's check of its names, under a mark of its OID that has
 * doltgres_libc_locale leave its libc names to Postgres' C library first; the
 * mark is cleared when the load ends, raising or not. A collation that copies
 * another's names answers nothing for Postgres to load it unmarked.
 */
bool
doltgres_entries_doltgres_new_collation(const PgCatalogCaps *caps, uint32 collid, const void *from, bool *result,
										PgPgError *error)
{
	bool		loaded;

	if (from != NULL)
	{
		*result = false;
		return true;
	}
	doltgres_entries_Loading_of(collid);
	loaded = pg_calls_load_collation(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), collid, error);
	doltgres_entries_Loading_drop();
	if (!loaded)
		return false;
	*result = true;
	return true;
}

/*
 * doltgres_entries_icu_locale_frame_0_t is icu_locale's state across its pad:
 * its arguments, the names it reads, their rules, and where the tag and the
 * outcome go.
 */
typedef struct doltgres_entries_icu_locale_frame_0_t
{
	const PgCatalogCaps *caps;
	uint32		collid;
	StdString	collate;
	bool		collate_live;
	StdString	ctype;
	bool		ctype_live;
	DoltgresLocaleRules rules;
	bool		rules_live;
	PgPgStringOption *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_icu_locale_frame_0_t;

/* doltgres_entries_icu_locale_said warns of a name's unknown part, or of a name of no form when part is NULL. */
static bool
doltgres_entries_icu_locale_said(const PgCatalogCaps *caps, StdStr name, const char *what, StdStr part, StdStr tag,
								 PgPgError *error)
{
	StdFmtPiece unknown[] = {STD_FMT_LIT("locale name \""), STD_FMT_ARG(0), STD_FMT_LIT("\" has unknown "),
	STD_FMT_ARG(1), STD_FMT_LIT(" \""), STD_FMT_ARG(2), STD_FMT_LIT("\"")};
	StdFmtPiece nameless[] = {STD_FMT_LIT("locale name \""), STD_FMT_ARG(0),
	STD_FMT_LIT("\" is not a known locale name")};
	StdStr		kind = {what, what != NULL ? strlen(what) : 0};
	StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&kind, std_str_Display_fmt}, {&part, std_str_Display_fmt}};
	StdFmtArguments arguments = {what != NULL ? unknown : nameless,
	what != NULL ? lengthof(unknown) : lengthof(nameless), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return doltgres_entries_warn(caps, &message, tag, error);
}

/* doltgres_entries_icu_locale_body_0 gives the constructor the tag the collation's names stand for. */
static void
doltgres_entries_icu_locale_body_0(void *frame_)
{
	static const char byte_order[] = "the ICU constructor was asked for a locale of byte order";
	doltgres_entries_icu_locale_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	int32		provider;
	StdStr		name;
	bool		quiet;
	StdStr		tag;
	StdStr		collate;
	StdStr		ctype;
	StdStr		none = {0};

	if (!pg_calls_locale_provider(raise, pg_CatalogCaps_catalog(frame->caps), frame->collid, &provider,
								  frame->error))
		return;
	if (provider != (int32) 'c')
	{
		frame->result->has = false;
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_locale_name(frame->caps, frame->collid, false, &frame->collate, frame->error))
		return;
	frame->collate_live = true;
	if (!doltgres_entries_locale_name(frame->caps, frame->collid, true, &frame->ctype, frame->error))
		return;
	frame->ctype_live = true;
	collate.ptr = frame->collate.ptr;
	collate.len = frame->collate.len;
	ctype.ptr = frame->ctype.ptr;
	ctype.len = frame->ctype.len;
	doltgres_locale_asked(collate, ctype, doltgres_entries_second_asked(), &name, &frame->rules);
	frame->rules_live = true;
	quiet = pg_calls_icu_validation_off();
	switch (frame->rules.tag)
	{
		case DOLTGRES_LOCALE_RULES_TAG:
			{
				StdString	owned;
				StdCString	validated;

				tag = std_cow_str_deref(&frame->rules.u.tag.tag);
				if (frame->rules.u.tag.has_unknown && !quiet)
				{
					DoltgresLocalePart *part = &frame->rules.u.tag.unknown;
					bool		modifier = part->tag == DOLTGRES_LOCALE_PART_MODIFIER;

					if (!doltgres_entries_icu_locale_said(frame->caps, name, modifier ? "modifier" : "sort name",
														  modifier ? part->u.modifier : part->u.sort, tag,
														  frame->error))
						return;
				}
				std_string_from_Checked(&owned, tag.ptr, tag.len);
				if (!doltgres_entries_c_string(&owned, &validated, frame->error))
					return;
				frame->ok = pg_calls_icu_validate_capped(raise, std_CString_as_ptr(&validated), frame->error);
				std_CString_drop(&validated);
				if (!frame->ok)
					return;
				break;
			}
		case DOLTGRES_LOCALE_RULES_UNKNOWN:
			if (!quiet && !doltgres_entries_icu_locale_said(frame->caps, name, NULL, none, STD_STR("und"),
															frame->error))
				return;
			tag = STD_STR("und");
			break;
		case DOLTGRES_LOCALE_RULES_C:
		case DOLTGRES_LOCALE_RULES_C_UTF8:
			*frame->error = doltgres_entries_internal(byte_order, sizeof(byte_order) - 1);
			frame->ok = false;
			return;
	}
	frame->ok = pg_calls_copy_string(raise, (const uint8 *) tag.ptr, tag.len, &frame->result->string, frame->error);
	frame->result->has = frame->ok;
}

/* doltgres_entries_icu_locale_pad_0 drops what an unwound ask holds. */
static void
doltgres_entries_icu_locale_pad_0(void *frame_)
{
	doltgres_entries_icu_locale_frame_0_t *frame = frame_;

	if (frame->rules_live)
		doltgres_locale_Rules_drop(&frame->rules);
	if (frame->ctype_live)
		std_String_drop(&frame->ctype);
	if (frame->collate_live)
		std_String_drop(&frame->collate);
}

/*
 * doltgres_entries_doltgres_icu_locale gives Postgres' ICU constructor the
 * tag of a libc locale it is asked to build, in place of the catalog's ICU
 * locale name, which a libc row leaves null; it answers nothing for a locale
 * of the ICU provider. The tag is the one the stored name stands for, the
 * collate name's or, on the libc site's second ask and where the collate name
 * asks for byte order, the ctype name's. A name of no form is ordered as the
 * root, and a modifier or a sort name the tag holds nothing for is left out
 * of it, each said in a warning of the fork's own, and a tag goes through
 * Postgres' own validation, both at icu_validation_level capped at a warning.
 */
bool
doltgres_entries_doltgres_icu_locale(const PgCatalogCaps *caps, uint32 collid, PgPgStringOption *result,
									 PgPgError *error)
{
	doltgres_entries_icu_locale_frame_0_t frame = {.caps = caps, .collid = collid, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_icu_locale_body_0, doltgres_entries_icu_locale_pad_0);
	doltgres_entries_icu_locale_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_builtin_frame_0_t is builtin_locale's state across its
 * pad: its arguments, the ctype name it reads, and where the name and the
 * outcome go.
 */
typedef struct doltgres_entries_builtin_frame_0_t
{
	const PgCatalogCaps *caps;
	uint32		collid;
	StdString	ctype;
	bool		ctype_live;
	PgPgStringOption *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_builtin_frame_0_t;

/* doltgres_entries_builtin_body_0 gives the builtin constructor the name the collation's ctype name stands for. */
static void
doltgres_entries_builtin_body_0(void *frame_)
{
	doltgres_entries_builtin_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	int32		provider;
	DoltgresLocaleRules rules;
	bool		unicode;
	StdStr		name;
	StdStr		ctype;

	if (!pg_calls_locale_provider(raise, pg_CatalogCaps_catalog(frame->caps), frame->collid, &provider,
								  frame->error))
		return;
	if (provider != (int32) 'c')
	{
		frame->result->has = false;
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_locale_name(frame->caps, frame->collid, true, &frame->ctype, frame->error))
		return;
	frame->ctype_live = true;
	ctype.ptr = frame->ctype.ptr;
	ctype.len = frame->ctype.len;
	rules = doltgres_locale_named(ctype);
	unicode = rules.tag == DOLTGRES_LOCALE_RULES_C_UTF8 && pg_calls_database_is_utf8();
	doltgres_locale_Rules_drop(&rules);
	name = unicode ? STD_STR("C.UTF-8") : STD_STR("C");
	frame->ok = pg_calls_copy_string(raise, (const uint8 *) name.ptr, name.len, &frame->result->string,
									 frame->error);
	frame->result->has = frame->ok;
}

/* doltgres_entries_builtin_pad_0 drops the ctype name an unwound ask read. */
static void
doltgres_entries_builtin_pad_0(void *frame_)
{
	doltgres_entries_builtin_frame_0_t *frame = frame_;

	if (frame->ctype_live)
		std_String_drop(&frame->ctype);
}

/*
 * doltgres_entries_doltgres_builtin_locale gives the builtin provider's
 * constructor the name of the builtin locale a libc locale of the `C` family
 * stands for (design item 12), in place of the catalog's locale name, which
 * a libc row leaves null: `C.UTF-8` for a `C.UTF-8` ctype name in a UTF-8
 * database, and `C` for any other. It answers nothing for a locale of the
 * builtin provider, whose name Postgres reads itself.
 */
bool
doltgres_entries_doltgres_builtin_locale(const PgCatalogCaps *caps, uint32 collid, PgPgStringOption *result,
										 PgPgError *error)
{
	doltgres_entries_builtin_frame_0_t frame = {.caps = caps, .collid = collid, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_builtin_body_0, doltgres_entries_builtin_pad_0);
	doltgres_entries_builtin_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_collversion_frame_0_t is libc_collversion's state across
 * its pad: its arguments, the name it reads, its rules, and where the version
 * and the outcome go.
 */
typedef struct doltgres_entries_collversion_frame_0_t
{
	const PgCatalogCaps *caps;
	const char *collcollate;
	StdString	name;
	bool		name_live;
	DoltgresLocaleRules rules;
	bool		rules_live;
	PgPgStringOption *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_collversion_frame_0_t;

/* doltgres_entries_collversion_body_0 gives the version of the tag the collate name stands for. */
static void
doltgres_entries_collversion_body_0(void *frame_)
{
	doltgres_entries_collversion_frame_0_t *frame = frame_;
	StdString	tag;
	StdCString	asked;
	StdStr		name;

	if (!doltgres_entries_utf8_of(frame->caps, frame->collcollate, &frame->name, frame->error))
		return;
	frame->name_live = true;
	name.ptr = frame->name.ptr;
	name.len = frame->name.len;
	frame->rules = doltgres_locale_named(name);
	frame->rules_live = true;
	switch (frame->rules.tag)
	{
		case DOLTGRES_LOCALE_RULES_C:
		case DOLTGRES_LOCALE_RULES_C_UTF8:
			frame->result->has = false;
			frame->ok = true;
			return;
		case DOLTGRES_LOCALE_RULES_TAG:
			std_CowStr_into_owned(&frame->rules.u.tag.tag, &tag);
			frame->rules_live = false;
			break;
		case DOLTGRES_LOCALE_RULES_UNKNOWN:
			std_string_from_Checked(&tag, "und", 3);
			doltgres_locale_Rules_drop(&frame->rules);
			frame->rules_live = false;
			break;
	}
	if (!doltgres_entries_c_string(&tag, &asked, frame->error))
		return;
	frame->ok = pg_calls_icu_collversion(pg_CatalogCaps_may_raise(frame->caps), std_CString_as_ptr(&asked),
										 &frame->result->string, frame->error);
	frame->result->has = frame->ok;
	std_CString_drop(&asked);
}

/* doltgres_entries_collversion_pad_0 drops what an unwound version holds. */
static void
doltgres_entries_collversion_pad_0(void *frame_)
{
	doltgres_entries_collversion_frame_0_t *frame = frame_;

	if (frame->rules_live)
		doltgres_locale_Rules_drop(&frame->rules);
	if (frame->name_live)
		std_String_drop(&frame->name);
}

/*
 * doltgres_entries_doltgres_libc_collversion gives the version of a libc
 * collation as the vendored collator's for the tag its collate name stands
 * for (design item 12), so an ICU upgrade flags every collation whose keys it
 * changes; it answers nothing for a name of the `C` family, whose version
 * Postgres' own code gives.
 */
bool
doltgres_entries_doltgres_libc_collversion(const PgCatalogCaps *caps, const char *collcollate,
										   PgPgStringOption *result, PgPgError *error)
{
	doltgres_entries_collversion_frame_0_t frame = {.caps = caps, .collcollate = collcollate, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_collversion_body_0, doltgres_entries_collversion_pad_0);
	doltgres_entries_collversion_pad_0(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
