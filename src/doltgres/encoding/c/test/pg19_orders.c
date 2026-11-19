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
 * encoding's pg19_orders test (encoding's tests/pg19_orders.rs): the keys of
 * values in their send forms, which PG19 recorded with each value's rank in
 * its own order, sort as Postgres sorts the values, ascending and
 * descending, with NULL placed as each order places it.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "storage/fd.h"
#include "utils/builtins.h"

#include "doltgres/encoding/key.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres_encoding_tests.h"

#define RUST_FILE "encoding" STD_SEP "tests" STD_SEP "pg19_orders.rs"

extern void test_encoding_pg19_orders_keys_sort_as_postgres_sorts_values(void);

/* Bytes is a range's bound value, a slice of its send form. */
#define ENCODING_KEY_SCOPE static
#define ENCODING_KEY_DECLARE
#define ENCODING_KEY_DEFINE
#define ENCODING_KEY_RANGE_TS Bytes
#define ENCODING_KEY_RANGE_T StdBytes
#define ENCODING_KEY_RANGE_T_COPY
#include "doltgres/encoding/key_template.h"

/* PgOrdersReader is Reader: the send form's bytes not read yet. */
typedef struct PgOrdersReader
{
	const uint8 *ptr;
	size_t		len;
} PgOrdersReader;

/* pg_orders_take takes the next n bytes, as split_at does, which panics past the end. */
static StdBytes
pg_orders_take(PgOrdersReader *r, size_t n)
{
	StdBytes	head;

	if (n > r->len)
		std_panic(RUST_FILE, 35, 36, "mid > len", sizeof("mid > len") - 1);
	head.ptr = r->ptr;
	head.len = n;
	r->ptr += n;
	r->len -= n;
	return head;
}

/* pg_orders_u8 reads a byte. */
static uint8
pg_orders_u8(PgOrdersReader *r)
{
	return pg_orders_take(r, 1).ptr[0];
}

/* pg_orders_be reads n bytes big-endian. */
static uint64
pg_orders_be(PgOrdersReader *r, size_t n)
{
	StdBytes	b = pg_orders_take(r, n);
	uint64		v = 0;

	for (size_t i = 0; i < n; i++)
		v = (v << 8) | b.ptr[i];
	return v;
}

/* pg_orders_field reads a length-prefixed field, false for a NULL. */
static bool
pg_orders_field(PgOrdersReader *r, StdBytes *field)
{
	int32		len = (int32) (uint32) pg_orders_be(r, 4);

	if (len == -1)
		return false;
	*field = pg_orders_take(r, (size_t) len);
	return true;
}

static void pg_orders_write_value(StdVec *out, const char *ty, StdBytes send);

/* pg_orders_digits makes Digits of decimal digits, unwrapped as the test's unwrap is. */
static EncodingKeyDigits
pg_orders_digits(const uint8 *digits, size_t len)
{
	EncodingKeyDigits made;

	if (!encoding_key_Digits_new(digits, len, &made))
		std_option_unwrap_failed(RUST_FILE, 60, 82);
	return made;
}

/* pg_orders_write_numeric appends a numeric's key from its send form. */
static void
pg_orders_write_numeric(StdVec *out, StdBytes send)
{
	PgOrdersReader r = {send.ptr, send.len};
	int16		ndigits = (int16) pg_orders_be(&r, 2);
	int16		weight = (int16) pg_orders_be(&r, 2);
	uint16		sign = (uint16) pg_orders_be(&r, 2);
	EncodingKeyNumeric value = {0};
	uint8	   *digits = palloc(Max(ndigits, 1) * 4);

	(void) pg_orders_be(&r, 2);
	for (int i = 0; i < ndigits; i++)
	{
		uint16		d = (uint16) pg_orders_be(&r, 2);

		digits[4 * i] = (uint8) (d / 1000);
		digits[4 * i + 1] = (uint8) (d / 100 % 10);
		digits[4 * i + 2] = (uint8) (d / 10 % 10);
		digits[4 * i + 3] = (uint8) (d % 10);
	}
	switch (sign)
	{
		case 0xc000:
			value.tag = ENCODING_KEY_NUMERIC_NA_N;
			break;
		case 0xd000:
			value.tag = ENCODING_KEY_NUMERIC_INFINITY;
			break;
		case 0xf000:
			value.tag = ENCODING_KEY_NUMERIC_NEGATIVE_INFINITY;
			break;
		default:
			value.tag = ENCODING_KEY_NUMERIC_FINITE;
			value.u.finite.negative = sign == 0x4000;
			value.u.finite.digits = pg_orders_digits(digits, (size_t) ndigits * 4);
			value.u.finite.exponent = 4 * (weight + 1);
			break;
	}
	encoding_key_numeric(out, &value);
	pfree(digits);
}

/* pg_orders_bound writes a range bound's value as its element type, whose name the captures are. */
static void
pg_orders_bound(void *captures, StdVec *out, StdBytes *value)
{
	pg_orders_write_value(out, captures, *value);
}

/* pg_orders_write_range appends a range's key from its send form, its bounds of the element type. */
static void
pg_orders_write_range(StdVec *out, StdBytes send, const char *element)
{
	PgOrdersReader r = {send.ptr, send.len};
	uint8		flags = pg_orders_u8(&r);
	encoding_key_rangeBytes_lower_t lower = {0};
	encoding_key_rangeBytes_lower_t upper = {0};
	bool		has_lower = (flags & 0x08) == 0;
	bool		has_upper = (flags & 0x10) == 0;

	if (flags & 0x01)
	{
		encoding_key_range_empty(out);
		return;
	}
	if (has_lower)
	{
		if (!pg_orders_field(&r, &lower.f0))
			std_option_unwrap_failed(RUST_FILE, 86, 63);
		lower.f1 = (flags & 0x02) != 0;
	}
	if (has_upper)
	{
		if (!pg_orders_field(&r, &upper.f0))
			std_option_unwrap_failed(RUST_FILE, 87, 63);
		upper.f1 = (flags & 0x04) != 0;
	}
	encoding_key_rangeBytes(out, has_lower, lower, has_upper, upper, pg_orders_bound, (void *) element, NULL);
}

/* pg_orders_write_array appends an array's key from its send form, its elements of the element type. */
static void
pg_orders_write_array(StdVec *out, StdBytes send, const char *element)
{
	PgOrdersReader r = {send.ptr, send.len};
	int32		ndim = (int32) (uint32) pg_orders_be(&r, 4);
	encoding_key_array_tail_dimensions_t *dims;
	int32		count = 1;

	(void) pg_orders_be(&r, 4);
	(void) pg_orders_be(&r, 4);
	dims = palloc(sizeof(*dims) * Max(ndim, 1));
	for (int i = 0; i < ndim; i++)
	{
		dims[i].f0 = (int32) (uint32) pg_orders_be(&r, 4);
		dims[i].f1 = (int32) (uint32) pg_orders_be(&r, 4);
		count *= dims[i].f0;
	}
	if (ndim <= 0)
		count = 0;
	for (int i = 0; i < count; i++)
	{
		StdBytes	e;

		if (!pg_orders_field(&r, &e))
			std_vec_u8_push_Checked(out, ENCODING_KEY_NULL_ELEMENT);
		else
		{
			std_vec_u8_push_Checked(out, ENCODING_KEY_ELEMENT);
			pg_orders_write_value(out, element, e);
		}
	}
	encoding_key_array_tail(out, dims, (size_t) Max(ndim, 0));
	pfree(dims);
}

/* PgOrdersJsonTag is Json's variants. */
typedef enum PgOrdersJsonTag
{
	PG_ORDERS_JSON_NULL,
	PG_ORDERS_JSON_BOOL,
	PG_ORDERS_JSON_NUMBER,
	PG_ORDERS_JSON_STRING,
	PG_ORDERS_JSON_ARRAY,
	PG_ORDERS_JSON_OBJECT,
} PgOrdersJsonTag;

/*
 * PgOrdersJson is Json: a jsonb value as its text form gives it, a number's
 * or a string's text, and an array's items or an object's keys and values,
 * the keys in jsonb's order.
 */
typedef struct PgOrdersJson
{
	PgOrdersJsonTag tag;
	bool		b;
	StdBytes text;
	struct PgOrdersJson *items;
	StdBytes   *keys;
	size_t		len;
} PgOrdersJson;

/* pg_orders_trim skips the whitespace at the text's start. */
static void
pg_orders_trim(StdBytes *text)
{
	while (text->len > 0 && (text->ptr[0] == ' ' || text->ptr[0] == '\t' || text->ptr[0] == '\n' ||
							 text->ptr[0] == '\r'))
	{
		text	  ->ptr++;
		text	  ->len--;
	}
}

/* pg_orders_skip skips n bytes of the text. */
static void
pg_orders_skip(StdBytes *text, size_t n)
{
	text	  ->ptr += n;
	text	  ->len -= n;
}

/* pg_orders_parse_string parses a JSON string without escapes. */
static StdBytes
pg_orders_parse_string(StdBytes *text)
{
	StdBytes	s;

	s.ptr = text->ptr + 1;

	s.len = 0;
	while (text->ptr[1 + s.len] != '"')
		s.len++;
	pg_orders_skip(text, s.len + 2);
	return s;
}

/* pg_orders_key_before orders an object's keys as jsonb stores them: by length, then by bytes. */
static bool
pg_orders_key_before(StdBytes a, StdBytes b)
{
	if (a.len != b.len)
		return a.len < b.len;
	return memcmp(a.ptr, b.ptr, a.len) < 0;
}

/* pg_orders_parse_json parses a JSON value from the text, leaving the rest. */
static PgOrdersJson
pg_orders_parse_json(StdBytes *text)
{
	PgOrdersJson value = {0};
	size_t		cap = 0;

	pg_orders_trim(text);
	switch (text->ptr[0])
	{
		case 'n':
			pg_orders_skip(text, 4);
			value.tag = PG_ORDERS_JSON_NULL;
			return value;
		case 't':
			pg_orders_skip(text, 4);
			value.tag = PG_ORDERS_JSON_BOOL;
			value.b = true;
			return value;
		case 'f':
			pg_orders_skip(text, 5);
			value.tag = PG_ORDERS_JSON_BOOL;
			return value;
		case '"':
			value.tag = PG_ORDERS_JSON_STRING;
			value.text = pg_orders_parse_string(text);
			return value;
		case '[':
		case '{':
			{
				bool		object = text->ptr[0] == '{';
				char		close = object ? '}' : ']';

				value.tag = object ? PG_ORDERS_JSON_OBJECT : PG_ORDERS_JSON_ARRAY;
				pg_orders_skip(text, 1);
				for (;;)
				{
					pg_orders_trim(text);
					if (text->ptr[0] == close)
					{
						pg_orders_skip(text, 1);
						break;
					}
					if (text->ptr[0] == ',')
					{
						pg_orders_skip(text, 1);
						pg_orders_trim(text);
					}
					if (value.len == cap)
					{
						cap = cap == 0 ? 4 : cap * 2;
						value.items = value.items ? repalloc(value.items, cap * sizeof(PgOrdersJson)) :
							palloc(cap * sizeof(PgOrdersJson));
						if (object)
							value.keys = value.keys ? repalloc(value.keys, cap * sizeof(StdBytes)) :
								palloc(cap * sizeof(StdBytes));
					}
					if (object)
					{
						value.keys[value.len] = pg_orders_parse_string(text);
						pg_orders_trim(text);
						pg_orders_skip(text, 1);
					}
					value.items[value.len] = pg_orders_parse_json(text);
					value.len++;
				}
				if (object)
				{
					for (size_t i = 1; i < value.len; i++)
						for (size_t j = i; j > 0 && pg_orders_key_before(value.keys[j], value.keys[j - 1]); j--)
						{
							StdBytes	k = value.keys[j];
							PgOrdersJson v = value.items[j];

							value.keys[j] = value.keys[j - 1];
							value.items[j] = value.items[j - 1];
							value.keys[j - 1] = k;
							value.items[j - 1] = v;
						}
				}
				return value;
			}
		default:
			value.tag = PG_ORDERS_JSON_NUMBER;
			value.text.ptr = text->ptr;

			while (value.text.len < text->len && strchr("+-.eE0123456789", text->ptr[value.text.len]) != NULL &&
				   text->ptr[value.text.len] != '\0')
				value.text.len++;
			pg_orders_skip(text, value.text.len);
			return value;
	}
}

/* pg_orders_write_decimal appends the key of a decimal number written as text. */
static void
pg_orders_write_decimal(StdVec *out, StdBytes text)
{
	size_t		at = 0;
	int32		exp = 0;
	size_t		mantissa_len;
	const uint8 *m = text.ptr;
	bool		negative;
	size_t		point;
	uint8	   *digits = palloc(text.len + 1);
	size_t		n = 0;
	EncodingKeyNumeric value = {ENCODING_KEY_NUMERIC_FINITE};

	while (at < text.len && text.ptr[at] != 'e' && text.ptr[at] != 'E')
		at++;
	mantissa_len = at;
	if (at < text.len)
	{
		char	   *exp_text = pnstrdup((const char *) text.ptr + at + 1, text.len - at - 1);

		exp = pg_strtoint32(exp_text);
		pfree(exp_text);
	}
	negative = mantissa_len > 0 && m[0] == '-';
	while (mantissa_len > 0 && (m[0] == '-' || m[0] == '+'))
	{
		m++;
		mantissa_len--;
	}
	point = mantissa_len;
	for (size_t i = 0; i < mantissa_len; i++)
		if (m[i] == '.')
		{
			point = i;
			break;
		}
	for (size_t i = 0; i < mantissa_len; i++)
		if (m[i] >= '0' && m[i] <= '9')
			digits[n++] = m[i] - '0';
	value.u.finite.negative = negative;
	value.u.finite.digits = pg_orders_digits(digits, n);
	value.u.finite.exponent = (int32) point + exp;
	encoding_key_numeric(out, &value);
	pfree(digits);
}

/* pg_orders_text writes a text's key, the captures its bytes, for jsonb_string. */
static void
pg_orders_text(void *captures, StdVec *out)
{
	const StdBytes *s = captures;

	encoding_key_text(out, s->ptr, s->len);
}

/* pg_orders_write_json appends a jsonb value's tokens. */
static void
pg_orders_write_json(StdVec *out, const PgOrdersJson *value)
{
	switch (value->tag)
	{
		case PG_ORDERS_JSON_NULL:
			encoding_key_jsonb_null(out);
			break;
		case PG_ORDERS_JSON_BOOL:
			encoding_key_jsonb_bool(out, value->b);
			break;
		case PG_ORDERS_JSON_NUMBER:
			std_vec_u8_push_Checked(out, 0x02);
			pg_orders_write_decimal(out, value->text);
			break;
		case PG_ORDERS_JSON_STRING:
			encoding_key_jsonb_string(out, pg_orders_text, (void *) &value->text, NULL);
			break;
		case PG_ORDERS_JSON_ARRAY:
			encoding_key_jsonb_array(out, (uint32) value->len, false);
			for (size_t i = 0; i < value->len; i++)
				pg_orders_write_json(out, &value->items[i]);
			break;
		case PG_ORDERS_JSON_OBJECT:
			encoding_key_jsonb_object(out, (uint32) value->len);
			for (size_t i = 0; i < value->len; i++)
			{
				encoding_key_jsonb_string(out, pg_orders_text, &value->keys[i], NULL);
				pg_orders_write_json(out, &value->items[i]);
			}
			break;
	}
}

/* pg_orders_is is whether the type's name is the text. */
static bool
pg_orders_is(const char *ty, const char *name)
{
	return strcmp(ty, name) == 0;
}

/* pg_orders_write_value appends the ascending key of a value of the type from its send form. */
static void
pg_orders_write_value(StdVec *out, const char *ty, StdBytes send)
{
	PgOrdersReader r = {send.ptr, send.len};

	if (pg_orders_is(ty, "bool"))
		encoding_key_bool(out, send.ptr[0] != 0);
	else if (pg_orders_is(ty, "char"))
		encoding_key_byte(out, send.ptr[0]);
	else if (pg_orders_is(ty, "int2"))
		encoding_key_i16(out, (int16) pg_orders_be(&r, 2));
	else if (pg_orders_is(ty, "int4") || pg_orders_is(ty, "date"))
		encoding_key_i32(out, (int32) (uint32) pg_orders_be(&r, 4));
	else if (pg_orders_is(ty, "int8") || pg_orders_is(ty, "timestamp"))
		encoding_key_i64(out, (int64) pg_orders_be(&r, 8));
	else if (pg_orders_is(ty, "oid"))
		encoding_key_u32(out, (uint32) pg_orders_be(&r, 4));
	else if (pg_orders_is(ty, "float4"))
	{
		uint32		bits = (uint32) pg_orders_be(&r, 4);
		float4		f;

		memcpy(&f, &bits, 4);
		encoding_key_f32(out, f);
	}
	else if (pg_orders_is(ty, "float8"))
	{
		uint64		bits = pg_orders_be(&r, 8);
		float8		f;

		memcpy(&f, &bits, 8);
		encoding_key_f64(out, f);
	}
	else if (pg_orders_is(ty, "numeric"))
		pg_orders_write_numeric(out, send);
	else if (pg_orders_is(ty, "text"))
		encoding_key_text(out, send.ptr, send.len);
	else if (pg_orders_is(ty, "bpchar"))
	{
		size_t		len = send.len;

		while (len > 0 && (send.ptr[len - 1] == ' ' || send.ptr[len - 1] == '\t' || send.ptr[len - 1] == '\n' ||
						   send.ptr[len - 1] == '\r' || send.ptr[len - 1] == '\f'))
			len--;
		encoding_key_text(out, send.ptr, len);
	}
	else if (pg_orders_is(ty, "bytea"))
		encoding_key_bytes(out, send.ptr, send.len);
	else if (pg_orders_is(ty, "varbit"))
	{
		uint32		len = (uint32) pg_orders_be(&r, 4);

		encoding_key_bits(out, r.ptr, r.len, len);
	}
	else if (pg_orders_is(ty, "uuid"))
		encoding_key_raw(out, send.ptr, send.len);
	else if (pg_orders_is(ty, "inet") || pg_orders_is(ty, "cidr"))
	{
		uint8		family = pg_orders_u8(&r);
		uint8		bits = pg_orders_u8(&r);
		uint8		len;
		StdBytes	address;

		(void) pg_orders_u8(&r);
		len = pg_orders_u8(&r);
		address = pg_orders_take(&r, len);
		encoding_key_inet(out, family == 3, bits, address.ptr, address.len);
	}
	else if (pg_orders_is(ty, "interval"))
	{
		int64		micros = (int64) pg_orders_be(&r, 8);
		int32		days = (int32) (uint32) pg_orders_be(&r, 4);
		int32		months = (int32) (uint32) pg_orders_be(&r, 4);

		encoding_key_interval(out, months, days, micros);
	}
	else if (pg_orders_is(ty, "timetz"))
	{
		int64		micros = (int64) pg_orders_be(&r, 8);
		int32		zone = (int32) (uint32) pg_orders_be(&r, 4);
		EncodingKeyTimeWithZone value;

		if (!encoding_key_TimeWithZone_new(micros, zone, &value))
			std_option_unwrap_failed(RUST_FILE, 263, 63);
		encoding_key_timetz(out, value);
	}
	else if (pg_orders_is(ty, "int4[]"))
		pg_orders_write_array(out, send, "int4");
	else if (pg_orders_is(ty, "text[]"))
		pg_orders_write_array(out, send, "text");
	else if (pg_orders_is(ty, "int4range"))
		pg_orders_write_range(out, send, "int4");
	else if (pg_orders_is(ty, "numrange"))
		pg_orders_write_range(out, send, "numeric");
	else if (pg_orders_is(ty, "int4multirange"))
	{
		int32		count = (int32) (uint32) pg_orders_be(&r, 4);

		for (int32 i = 0; i < count; i++)
		{
			StdBytes	f;

			std_vec_u8_push_Checked(out, ENCODING_KEY_ELEMENT);
			if (!pg_orders_field(&r, &f))
				std_option_unwrap_failed(RUST_FILE, 272, 61);
			pg_orders_write_value(out, "int4range", f);
		}
		std_vec_u8_push_Checked(out, ENCODING_KEY_END);
	}
	else if (pg_orders_is(ty, "pair"))
	{
		static const char *const types[] = {"int4", "text"};
		size_t		count = (size_t) (uint32) pg_orders_be(&r, 4);

		for (size_t i = 0; i < Min(count, lengthof(types)); i++)
		{
			StdBytes	f;

			(void) pg_orders_be(&r, 4);
			if (!pg_orders_field(&r, &f))
				std_vec_u8_push_Checked(out, ENCODING_KEY_NULL_ELEMENT);
			else
			{
				std_vec_u8_push_Checked(out, ENCODING_KEY_ELEMENT);
				pg_orders_write_value(out, types[i], f);
			}
		}
		std_vec_u8_push_Checked(out, ENCODING_KEY_END);
	}
	else if (pg_orders_is(ty, "jsonb"))
	{
		StdBytes text = {send.ptr + 1, send.len - 1};
		PgOrdersJson value = pg_orders_parse_json(&text);

		if (value.tag != PG_ORDERS_JSON_ARRAY && value.tag != PG_ORDERS_JSON_OBJECT)
			encoding_key_jsonb_array(out, 1, true);
		pg_orders_write_json(out, &value);
	}
	else
	{
		char	   *message = psprintf("no key encoding for %s", ty);

		std_panic(RUST_FILE, 301, 14, message, strlen(message));
	}
}

/* PgOrdersCaptures is a column's write's captures: the value's type and its send form. */
typedef struct PgOrdersCaptures
{
	const char *ty;
	StdBytes	send;
} PgOrdersCaptures;

/* pg_orders_column_write writes a column's value for key::column. */
static void
pg_orders_column_write(void *captures, StdVec *out)
{
	PgOrdersCaptures *c = captures;

	pg_orders_write_value(out, c->ty, c->send);
}

/* pg_orders_cmp is Vec<u8>'s Ord: the bytes, then the lengths. */
static StdOrdering
pg_orders_cmp(const StdVec *a, const StdVec *b)
{
	int			c = memcmp(a->ptr, b->ptr, Min(a->len, b->len));

	if (c != 0)
		return c < 0 ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	return a->len < b->len ? STD_ORDERING_LESS : (a->len > b->len ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* pg_orders_Ordering_Debug_fmt is Ordering's Debug. */
static bool
pg_orders_Ordering_Debug_fmt(const void *value, StdFormatter *f)
{
	StdOrdering o = *(const StdOrdering *) value;
	const char *name = o == STD_ORDERING_LESS ? "Less" : (o == STD_ORDERING_EQUAL ? "Equal" : "Greater");

	return std_fmt_write_str(f, name, strlen(name));
}

/* pg_orders_hex decodes a hexadecimal digit. */
static uint8
pg_orders_hex(char c)
{
	return (uint8) (c >= 'a' ? c - 'a' + 10 : (c >= 'A' ? c - 'A' + 10 : c - '0'));
}

/* PgOrdersValue is Ranked: a value's rank in Postgres' order and its send form. */
typedef struct PgOrdersValue
{
	uint32		rank;
	StdBytes	send;
	StdVec		key;
} PgOrdersValue;

/* PgOrdersType is a recorded type: its name and its values in the table's order. */
typedef struct PgOrdersType
{
	char	   *name;
	PgOrdersValue *values;
	size_t		len;
	size_t		cap;
} PgOrdersType;

/*
 * test_encoding_pg19_orders_keys_sort_as_postgres_sorts_values reads PG19's
 * recorded orders and checks, for every type the encoding crate keys, that
 * its values' keys sort as their ranks do, ascending and descending, and
 * that NULL's key sorts first or last as the order places NULL.
 */
void
test_encoding_pg19_orders_keys_sort_as_postgres_sorts_values(void)
{
	static const char path[] = DOLTGRES_ENCODING_TESTS "/pg19_orders.tsv";
	StringInfoData file;
	FILE	   *f;
	char		buf[4096];
	size_t		got;
	size_t		types_cap = 16;
	PgOrdersType *types = palloc(sizeof(PgOrdersType) * types_cap);
	size_t		ntypes = 0;
	const char *text;
	const char *end;

	f = AllocateFile(path, PG_BINARY_R);
	if (f == NULL)
		std_panic(RUST_FILE, 316, 18, "couldn't read pg19_orders.tsv", sizeof("couldn't read pg19_orders.tsv") - 1);
	initStringInfo(&file);
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
		appendBinaryStringInfo(&file, buf, (int) got);
	FreeFile(f);
	text = file.data;
	end = text +file.len;

	while (text <end)
	{
		const char *line_end = memchr(text, '\n', end - text);
		const char *tab1;
		const char *tab2;
		PgOrdersValue *v;
		PgOrdersType *t;

		if (line_end == NULL)
			line_end = end;
		tab1 = memchr(text, '\t', line_end - text);
		tab2 = memchr(tab1 + 1, '\t', line_end - tab1 - 1);
		if (ntypes == 0 || strlen(types[ntypes - 1].name) != (size_t) (tab1 - text) ||
			memcmp(types[ntypes - 1].name, text, tab1 - text) != 0)
		{
			if (ntypes == types_cap)
			{
				types_cap *= 2;
				types = repalloc(types, sizeof(PgOrdersType) * types_cap);
			}
			types[ntypes].name = pnstrdup(text, tab1 - text);
			types[ntypes].cap = 16;
			types[ntypes].values = palloc(sizeof(PgOrdersValue) * types[ntypes].cap);
			types[ntypes].len = 0;
			ntypes++;
		}
		t = &types[ntypes - 1];
		if (t->len == t->cap)
		{
			t->cap *= 2;
			t->values = repalloc(t->values, sizeof(PgOrdersValue) * t->cap);
		}
		v = &t->values[t->len++];
		{
			char	   *rank = pnstrdup(tab1 + 1, tab2 - tab1 - 1);
			size_t		hex_len = line_end - tab2 - 1;
			uint8	   *bytes = palloc(hex_len / 2 + 1);

			v->rank = (uint32) pg_strtoint32(rank);
			pfree(rank);
			for (size_t i = 0; i < hex_len / 2; i++)
				bytes[i] = (uint8) (pg_orders_hex(tab2[1 + 2 * i]) << 4 | pg_orders_hex(tab2[2 + 2 * i]));
			v->send.ptr = bytes;
			v->send.len = hex_len / 2;
		}
		text = line_end + 1;
	}
	if (!(ntypes >= 27))
		std_panic(RUST_FILE, 325, 5, "assertion failed: types.len() >= 27",
				  sizeof("assertion failed: types.len() >= 27") - 1);
	for (size_t ti = 0; ti < ntypes; ti++)
	{
		PgOrdersType *t = &types[ti];
		EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, ENCODING_KEY_ORDER_DESC};

		if (strcmp(t->name, "tsvector") == 0 || strcmp(t->name, "tsquery") == 0)
			continue;
		for (int oi = 0; oi < 2; oi++)
		{
			EncodingKeyOrder order = orders[oi];
			StdVec		null = STD_VEC_NEW;

			for (size_t i = 0; i < t->len; i++)
			{
				PgOrdersCaptures c = {t->name, t->values[i].send};

				t->values[i].key = (StdVec) STD_VEC_NEW;
				encoding_key_column(&t->values[i].key, order, pg_orders_column_write, &c, NULL);
			}
			for (size_t ai = 0; ai < t->len; ai++)
				for (size_t bi = 0; bi < t->len; bi++)
				{
					PgOrdersValue *a = &t->values[ai];
					PgOrdersValue *b = &t->values[bi];
					uint32		x = order.descending ? b->rank : a->rank;
					uint32		y = order.descending ? a->rank : b->rank;
					StdOrdering expected = x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
					StdOrdering got = pg_orders_cmp(&a->key, &b->key);

					if (got != expected)
					{
						StdStr		name = {t->name, strlen(t->name)};
						StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1),
						STD_FMT_LIT(": ranks "), STD_FMT_ARG(2), STD_FMT_LIT(" and "), STD_FMT_ARG(3)};
						StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&order, encoding_key_Order_Debug_fmt},
						{&a->rank, std_u32_Display_fmt}, {&b->rank, std_u32_Display_fmt}};
						StdFmtArguments message = {pieces, 7, args};

						std_assert_failed(RUST_FILE, 340, 21, "==", &got, pg_orders_Ordering_Debug_fmt, &expected,
										  pg_orders_Ordering_Debug_fmt, &message);
					}
				}
			encoding_key_null(&null, order);
			for (size_t i = 0; i < t->len; i++)
				if ((pg_orders_cmp(&null, &t->values[i].key) == STD_ORDERING_LESS) != order.nulls_first)
				{
					char	   *message = psprintf("%s: NULL is misplaced", t->name);

					std_panic(RUST_FILE, 345, 13, message, strlen(message));
				}
			std_vec_free_Checked(&null, 1, 1);
			for (size_t i = 0; i < t->len; i++)
				std_vec_free_Checked(&t->values[i].key, 1, 1);
		}
	}
	pfree(file.data);
}
