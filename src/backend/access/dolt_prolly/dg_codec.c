/*
 * Schema-driven storage serialization. SQL comparison is the index AM's job.
 * Preserve the original six encodings; new binary fields have a DGB1 envelope.
 * Arrays/composites recurse so nested TOAST pointers and padding never persist.
 */
#include "dg.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include "access/htup_details.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type.h"
#include "common/int128.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/inet.h"
#include "utils/varbit.h"
#include "utils/jsonb.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"
#include "utils/pg_locale.h"
#include "catalog/pg_collation_d.h"
#include "executor/executor.h"
#include "utils/expandeddatum.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "utils/typcache.h"
#include "utils/xml.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "catalog/catalogview.h"
#include "catalog/dependency.h"
#include "commands/extension.h"
#include "utils/memutils.h"
#include "common/hashfn.h"

static PLSlice encode_value(Oid type, Datum value);
static Datum decode_value(Oid type, PLSlice bytes);

static PLSlice
pack_fields(PLValue * fields, int count)
{
	uint8	   *packed;
	size_t		length;
	PLSlice		result;

	dg_check(pl_tuple_pack(fields, count, &packed, &length));
	result.data = palloc(length);
	result.len = length;
	memcpy((void *) result.data, packed, length);
	free(packed);
	return result;
}

/*
 * Validate schema, not just non-NULL datums. The recursive codecs strip array
 * element/composite type OIDs; arbitrary binary senders need not do so. Only
 * audited built-in binary formats and enum labels may reach that fallback.
 * Do not cache this check: ALTER TYPE can change a nested composite descriptor.
 */
/*
 * Extension types whose binary send output has been reviewed: a value's own
 * contents, with no local identifier, so a server with the extension reads
 * it back. PostGIS sends geometry and geography as extended well-known
 * binary, whose spatial reference is an SRID, a value the data defines.
 */
static const struct
{
	const char *extension;
	const char *type;
}			reviewed_extension_types[] = {
	{"postgis", "geometry"},
	{"postgis", "geography"},
};

/* Whether |type| is an extension's type listed in reviewed_extension_types. */
static bool
reviewed_extension_type(Oid type)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	bool		reviewed = false;

	PG_TRY();
	{
		Oid			extension = getExtensionOfObject(TypeRelationId, type);
		char	   *extname = OidIsValid(extension) ? get_extension_name(extension) : NULL;
		HeapTuple	tuple = extname ? SearchSysCache1(TYPEOID, ObjectIdGetDatum(type)) : NULL;

		if (HeapTupleIsValid(tuple))
		{
			const char *typname = NameStr(((Form_pg_type) GETSTRUCT(tuple))->typname);

			for (int i = 0; i < lengthof(reviewed_extension_types); i++)
				reviewed |= !strcmp(reviewed_extension_types[i].extension, extname) &&
					!strcmp(reviewed_extension_types[i].type, typname);
			ReleaseSysCache(tuple);
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return reviewed;
}

static void
check_type_recursive(Oid type, List *ancestors)
{
	Oid			element;
	char		kind;

	check_stack_depth();
	type = getBaseType(type);
	if (list_member_oid(ancestors, type))
		return;
	ancestors = lappend_oid(list_copy(ancestors), type);
	element = get_element_type(type);
	kind = get_typtype(type);
	if (OidIsValid(element))
		check_type_recursive(element, ancestors);
	else if (kind == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
		int			i;

		for (i = 0; i < desc->natts; i++)
			if (!TupleDescAttr(desc, i)->attisdropped)
				check_type_recursive(TupleDescAttr(desc, i)->atttypid, ancestors);
		ReleaseTupleDesc(desc);
	}

	/*
	 * A range sends its flags and bounds, never its type OID, so it is as
	 * portable as its subtype; a subtype's own sender may embed OIDs.
	 */
	else if (kind == TYPTYPE_RANGE)
	{
		Oid			subtype = getBaseType(get_range_subtype(type));

		/* Nested in a range, arrays and records send their own type OIDs. */
		if (OidIsValid(get_element_type(subtype)) || get_typtype(subtype) == TYPTYPE_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prolly storage rejects type %s", format_type_be(type)),
					 errdetail("Only codecs verified to exclude non-portable identifiers are supported."),
					 errhint("OID and other local identifier translation is not implemented.")));
		check_type_recursive(subtype, ancestors);
	}
	else if (kind == TYPTYPE_MULTIRANGE)
		check_type_recursive(get_multirange_range(type), ancestors);
	else if (kind != TYPTYPE_ENUM)
	{
		switch (type)
		{
				/*
				 * Scalar codecs serialize explicit SQL values, including identifiers
				 * requested by the user, but never incidental runtime headers.
				 */
			case REGPROCOID:
			case REGPROCEDUREOID:
			case REGOPEROID:
			case REGOPERATOROID:
			case REGCLASSOID:
			case REGCOLLATIONOID:
			case REGTYPEOID:
			case REGROLEOID:
			case REGNAMESPACEOID:
			case REGCONFIGOID:
			case REGDICTIONARYOID:
			case XIDOID:
			case XID8OID:
			case CIDOID:
			case TIDOID:
			case LSNOID:
			case TXID_SNAPSHOTOID:
			case PG_SNAPSHOTOID:
			case REFCURSOROID:
			case ACLITEMOID:
			case PG_NODE_TREEOID:
			case OIDOID: /* User data, not an incidental encoding header. */
			case BOOLOID:
			case BYTEAOID:
			case CHAROID:
			case NAMEOID:
			case INT2OID:
			case INT4OID:
			case INT8OID:
			case TEXTOID:
			case BPCHAROID:
			case VARCHAROID:
			case FLOAT4OID:
			case FLOAT8OID:
			case NUMERICOID:
			case CASHOID:
			case DATEOID:
			case TIMEOID:
			case TIMETZOID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
			case INTERVALOID:
			case BITOID:
			case VARBITOID:
			case UUIDOID:
			case INETOID:
			case CIDROID:
			case MACADDROID:
			case MACADDR8OID:
			case POINTOID:
			case LSEGOID:
			case PATHOID:
			case BOXOID:
			case POLYGONOID:
			case LINEOID:
			case CIRCLEOID:
			case JSONOID:
			case JSONBOID:
			case JSONPATHOID:
			case XMLOID:
			case TSVECTOROID:
			case TSQUERYOID:
				break;
			default:
				if (type >= FirstNormalObjectId && reviewed_extension_type(type))
					break;
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("prolly storage rejects type %s", format_type_be(type)),
						 errdetail("Only codecs verified to exclude non-portable identifiers are supported."),
						 errhint("OID and other local identifier translation is not implemented.")));
		}
	}
	list_free(ancestors);
}

static void
check_type(Oid type)
{
	check_type_recursive(type, NIL);
}

void
dg_validate_descriptor(TupleDesc desc)
{
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		check_type(TupleDescAttr(desc, i)->atttypid);
	}
}

/*
 * Binary I/O normally uses client_encoding; storage must use database encoding.
 * Restore backend conversion state even if a type function throws. xml_recv's
 * CONTENT mode accepts both XML documents and fragments, independent of the
 * reading session's xmloption. Domain constraints were checked by the executor
 * on assignment and must not be checked again when reading stored values.
 */
static bytea *
send_value(Oid type, Datum value)
{
	Oid			function;
	bool		varlena;
	int			encoding = pg_get_client_encoding();
	bytea	   *result = NULL;

	getTypeBinaryOutputInfo(type, &function, &varlena);
	PG_TRY();
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		result = OidSendFunctionCall(function, value);
	}
	PG_FINALLY();
	{
		if (SetClientEncoding(encoding) < 0)
			elog(FATAL, "could not restore client encoding");
	}
	PG_END_TRY();
	return result;
}

static Datum
receive_value(Oid type, PLSlice bytes)
{
	Oid			function,
				parameter;
	StringInfoData input;
	int			encoding = pg_get_client_encoding();
	int			saved_xmloption = xmloption;
	Datum		result = (Datum) 0;

	/* PostgreSQL 18's tsqueryrecv rejects the valid zero-item wire form. */
	if (type == TSQUERYOID && bytes.len == 4 && pl_read32(bytes.data) == 0)
	{
		TSQuery		query = palloc0(HDRSIZETQ);

		SET_VARSIZE(query, HDRSIZETQ);
		return PointerGetDatum(query);
	}
	getTypeBinaryInputInfo(type, &function, &parameter);
	initStringInfo(&input);
	appendBinaryStringInfo(&input, (const char *) bytes.data, bytes.len);
	PG_TRY();
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		xmloption = XMLOPTION_CONTENT;
		result = OidReceiveFunctionCall(function, &input, parameter, -1);
		if (input.cursor != input.len)
			elog(ERROR, "trailing bytes in prolly binary value");
	}
	PG_FINALLY();
	{
		xmloption = saved_xmloption;
		if (SetClientEncoding(encoding) < 0)
			elog(FATAL, "could not restore client encoding");
	}
	PG_END_TRY();
	pfree(input.data);
	return result;
}

static PLSlice
encode_array(Oid element, Datum value)
{
	ArrayType  *array = DatumGetArrayTypeP(value);
	Datum	   *values;
	bool	   *nulls;
	int			count,
				i,
				ndim = ARR_NDIM(array);
	int16		length;
	bool		byval;
	char		align;
	PLValue    *fields;
	uint8	   *shape = palloc(4 + 8 * ndim);
	PLSlice		result;

	get_typlenbyvalalign(element, &length, &byval, &align);
	deconstruct_array(array, element, length, byval, align, &values, &nulls, &count);
	fields = palloc0(sizeof(*fields) * (count + 1));
	pl_write32(shape, ndim);
	for (i = 0; i < ndim; i++)
	{
		pl_write32(shape + 4 + 8 * i, ARR_DIMS(array)[i]);
		pl_write32(shape + 8 + 8 * i, ARR_LBOUND(array)[i]);
	}
	fields[0].present = true;
	fields[0].bytes = (PLSlice)
	{
		shape, 4 + 8 * ndim
	};
	for (i = 0; i < count; i++)
	{
		fields[i + 1].present = !nulls[i];
		if (!nulls[i])
			fields[i + 1].bytes = encode_value(element, values[i]);
	}
	result = pack_fields(fields, count + 1);
	pfree(fields);
	pfree(values);
	pfree(nulls);
	return result;
}

static Datum
decode_array(Oid type, Oid element, PLSlice bytes)
{
	PLValue		shape,
				field;
	int			dims[MAXDIM],
				lbs[MAXDIM];
	int			ndim,
				count,
				i;
	Datum	   *values;
	bool	   *nulls;
	int16		length;
	bool		byval;
	char		align;
	ArrayType  *result;

	dg_check(pl_tuple_get(bytes, 0, &shape));
	if (!shape.present || shape.bytes.len < 4)
		elog(ERROR, "invalid prolly array shape");
	ndim = pl_read32(shape.bytes.data);
	if (ndim < 0 || ndim > MAXDIM || shape.bytes.len != 4 + 8 * ndim)
		elog(ERROR, "invalid prolly array dimensions");
	for (i = 0; i < ndim; i++)
	{
		dims[i] = pl_read32(shape.bytes.data + 4 + 8 * i);
		lbs[i] = (int32) pl_read32(shape.bytes.data + 8 + 8 * i);
	}
	count = ArrayGetNItems(ndim, dims);
	ArrayCheckBounds(ndim, dims, lbs);
	if (pl_read32(bytes.data + 8) != (uint32) count + 1)
		elog(ERROR, "invalid prolly array element count");
	/* Catalog vectors keep one zero-length dimension, unlike empty arrays. */
	if (count == 0 && type == INT2VECTOROID)
		return PointerGetDatum(buildint2vector(NULL, 0));
	if (count == 0 && type == OIDVECTOROID)
		return PointerGetDatum(buildoidvector(NULL, 0));
	values = palloc0(sizeof(*values) * Max(count, 1));
	nulls = palloc(sizeof(*nulls) * Max(count, 1));
	for (i = 0; i < count; i++)
	{
		dg_check(pl_tuple_get(bytes, i + 1, &field));
		nulls[i] = !field.present;
		if (field.present)
			values[i] = decode_value(element, field.bytes);
	}
	get_typlenbyvalalign(element, &length, &byval, &align);
	result = construct_md_array(values, nulls, ndim, dims, lbs, element, length, byval, align);
	for (i = 0; i < count; i++)
		if (!nulls[i] && !byval)
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	return PointerGetDatum(result);
}

static PLSlice
encode_record(Oid type, Datum value)
{
	HeapTupleHeader header = DatumGetHeapTupleHeader(value);
	HeapTupleData tuple;
	TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
	Datum	   *values = palloc(sizeof(*values) * desc->natts);
	bool	   *nulls = palloc(sizeof(*nulls) * desc->natts);
	PLValue    *fields = palloc0(sizeof(*fields) * desc->natts);
	PLSlice		result;
	int			i;

	tuple.t_len = HeapTupleHeaderGetDatumLength(header);
	tuple.t_data = header;
	heap_deform_tuple(&tuple, desc, values, nulls);
	for (i = 0; i < desc->natts; i++)
	{
		fields[i].present = !nulls[i] && !TupleDescAttr(desc, i)->attisdropped;
		if (fields[i].present)
			fields[i].bytes = encode_value(TupleDescAttr(desc, i)->atttypid, values[i]);
	}
	result = pack_fields(fields, desc->natts);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);
	pfree(fields);
	return result;
}

static HeapTuple
decode_fields(TupleDesc desc, PLSlice bytes)
{
	Datum	   *values = palloc0(sizeof(*values) * desc->natts);
	bool	   *nulls = palloc(sizeof(*nulls) * desc->natts);
	HeapTuple	result;
	PLTupleView view;
	int			i;

	dg_check(pl_tuple_open(bytes, &view));
	if (view.count != (size_t) desc->natts)
		dg_unsupported("schema changes to stored tuples");
	for (i = 0; i < desc->natts; i++)
	{
		PLValue		field;

		dg_check(pl_tuple_field(&view, i, &field));
		nulls[i] = !field.present || TupleDescAttr(desc, i)->attisdropped;
		if (!nulls[i])
			values[i] = decode_value(TupleDescAttr(desc, i)->atttypid, field.bytes);
	}
	result = heap_form_tuple(desc, values, nulls);
	for (i = 0; i < desc->natts; i++)
		if (!nulls[i] && !TupleDescAttr(desc, i)->attbyval)
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	return result;
}

static PLSlice
encode_value(Oid type, Datum value)
{
	PLSlice		result;
	Oid			element;
	uint8	   *data;
	bytea	   *binary;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID || type == PG_NODE_TREEOID)
	{
		struct varlena *v = PG_DETOAST_DATUM_PACKED(value);

		return (PLSlice)
		{
			(uint8 *) VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v)
		};
	}
	if (type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID)
	{
		int64		n = type == BOOLOID ? DatumGetBool(value) : type == INT2OID ? DatumGetInt16(value) : type == INT4OID ? DatumGetInt32(value) : DatumGetInt64(value);

		data = palloc(8);
		pl_key_int64(n, data);
		return (PLSlice)
		{
			data, 8
		};
	}

	if (type == ACLITEMOID)
	{
		AclItem *acl = DatumGetAclItemP(value);

		data = palloc(20);
		memcpy(data, "DGA1", 4);
		pl_write32(data + 4, acl->ai_grantee);
		pl_write32(data + 8, acl->ai_grantor);
		pl_write64(data + 12, acl->ai_privs);
		return (PLSlice) {data, 20};
	}
	element = get_element_type(type);
	if (OidIsValid(element))
		return encode_array(element, value);
	if (get_typtype(type) == TYPTYPE_COMPOSITE)
		return encode_record(type, value);
	check_type(type);
	binary = send_value(type, value);
	result.len = 4 + VARSIZE_ANY_EXHDR(binary);
	data = palloc(result.len);
	memcpy(data, "DGB1", 4);
	memcpy(data + 4, VARDATA_ANY(binary), result.len - 4);
	result.data = data;
	pfree(binary);
	return result;
}

static Datum
decode_value(Oid type, PLSlice bytes)
{
	Oid			element;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID || type == PG_NODE_TREEOID)
	{
		struct varlena *v = palloc(VARHDRSZ + bytes.len);

		SET_VARSIZE(v, VARHDRSZ + bytes.len);
		memcpy(VARDATA(v), bytes.data, bytes.len);
		return PointerGetDatum(v);
	}
	if (type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID)
	{
		int64		n;

		if (bytes.len != 8)
			elog(ERROR, "invalid prolly integer encoding");
		n = (int64) (pl_read64(bytes.data) ^ UINT64CONST(0x8000000000000000));
		return type == BOOLOID ? BoolGetDatum(n != 0) : type == INT2OID ? Int16GetDatum(n) : type == INT4OID ? Int32GetDatum(n) : Int64GetDatum(n);
	}

	if (type == ACLITEMOID)
	{
		AclItem *acl = palloc(sizeof(AclItem));

		if (bytes.len != 20 || memcmp(bytes.data, "DGA1", 4))
			elog(ERROR, "invalid prolly ACL item encoding");
		acl->ai_grantee = pl_read32(bytes.data + 4);
		acl->ai_grantor = pl_read32(bytes.data + 8);
		acl->ai_privs = pl_read64(bytes.data + 12);
		return PointerGetDatum(acl);
	}
	element = get_element_type(type);
	if (OidIsValid(element))
		return decode_array(type, element, bytes);
	if (get_typtype(type) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
		HeapTuple	tuple = decode_fields(desc, bytes);
		Datum		result = heap_copy_tuple_as_datum(tuple, desc);

		heap_freetuple(tuple);
		ReleaseTupleDesc(desc);
		return result;
	}
	if (bytes.len < 4 || memcmp(bytes.data, "DGB1", 4))
		elog(ERROR, "invalid prolly binary codec version");
	bytes.data += 4;
	bytes.len -= 4;
	return receive_value(type, bytes);
}

PLSlice
dg_encode(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext, "prolly codec", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	PLValue    *fields;
	PLSlice		packed,
				result;
	int			i;

	dg_validate_descriptor(desc);
	slot_getallattrs(slot);
	old = MemoryContextSwitchTo(scratch);
	fields = palloc0(sizeof(*fields) * desc->natts);
	for (i = 0; i < desc->natts; i++)
	{
		fields[i].present = !slot->tts_isnull[i];
		if (fields[i].present)
			fields[i].bytes = encode_value(TupleDescAttr(desc, i)->atttypid, slot->tts_values[i]);
	}
	packed = pack_fields(fields, desc->natts);
	MemoryContextSwitchTo(old);
	result.data = palloc(packed.len);
	result.len = packed.len;
	memcpy((void *) result.data, packed.data, packed.len);
	MemoryContextDelete(scratch);
	return result;
}

/* Native ALTER scans may request the old descriptor for an expression rewrite
 * or the intermediate descriptor for constraint validation. Normalize only the
 * transient tuple shape; canonical publication chooses the final live columns. */
void
dg_decode_mutation(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	PLValue    *fields = palloc0_array(PLValue, Max(desc->natts, 1));
	uint8	   *packed;
	size_t		length;
	HeapTuple	tuple;
	uint32		count;
	PLSlice		normalized;

	if (bytes.len < 12)
		elog(ERROR, "invalid provisional source row");
	count = pl_read32(bytes.data + 8);
	for (int i = 0; i < desc->natts && i < count; i++)
		dg_check(pl_tuple_get(bytes, i, &fields[i]));
	dg_check(pl_tuple_pack(fields, desc->natts, &packed, &length));
	normalized.data = packed;
	normalized.len = length;
	tuple = dg_decode_descriptor(desc, normalized);
	free(packed);
	pfree(fields);
	tuple->t_self = *tid;
	tuple->t_tableOid = RelationGetRelid(rel);
	ExecForceStoreHeapTuple(tuple, slot, true);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}

/* ---------------------------------------------------------------------------
 * Decode plans
 * ---------------------------------------------------------------------------
 *
 * Decoding a row used to validate the relation's descriptor and look every
 * column's type up again, per row. A decode plan does that once per relation:
 * it validates the descriptor, classifies each column, and resolves the
 * receive function of those that need one. Plans are keyed by relation OID
 * and dropped by relcache invalidation; a descriptor fingerprint guards the
 * private descriptors that share an OID across branches.
 */
typedef enum DecodeKind
{
	DECODE_INTEGER,				/* bool and the integer types */
	DECODE_BYTES,				/* text and bytea */
	DECODE_ENVELOPE_BYTES,		/* varchar, bpchar, json: the text in an envelope */
	DECODE_WIRE8,				/* an 8-byte by-value wire form: timestamps, float8, money */
	DECODE_WIRE4,				/* a 4-byte by-value wire form: date, float4 */
	DECODE_UUID,				/* 16 raw bytes */
	DECODE_NUMERIC,				/* the numeric wire form, assembled directly */
	DECODE_GENERAL,				/* arrays and composites: the recursive codec */
	DECODE_RECEIVE				/* a binary receive function */
} DecodeKind;

typedef struct DecodeColumn
{
	Oid			type;			/* the column's declared type */
	Oid			base;			/* its base type, which the codec uses */
	DecodeKind	kind;
	FmgrInfo	receive;
	Oid			ioparam;
	bool		dropped;
} DecodeColumn;

typedef struct DecodePlan
{
	Oid			relid;			/* hash key */
	TupleDesc	desc;
	bool		relcache;		/* desc is the relation's cached descriptor */
	uint32		fingerprint;
	int			natts;
	DecodeColumn *columns;
	bool		xml;			/* any column receives XML */
} DecodePlan;

static HTAB *decode_plans;
static DecodePlan *last_plan;	/* the plan last returned, checked first */
static bool decode_callback;

static void
decode_plans_changed(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	DecodePlan *plan;

	if (!decode_plans)
		return;
	hash_seq_init(&status, decode_plans);
	while ((plan = hash_seq_search(&status)) != NULL)
		if (!OidIsValid(relid) || plan->relid == relid)
		{
			if (plan == last_plan)
				last_plan = NULL;
			pfree(plan->columns);
			hash_search(decode_plans, &plan->relid, HASH_REMOVE, NULL);
		}
}

/* The descriptor's shape, to notice a private descriptor replaced in place. */
static uint32
descriptor_fingerprint(TupleDesc desc)
{
	uint32		hash = (uint32) desc->natts;
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		hash = hash_combine(hash, (uint32) attr->atttypid);
		hash = hash_combine(hash, (uint32) attr->atttypmod);
		hash = hash_combine(hash, (uint32) attr->attisdropped);
	}
	return hash;
}

static void
decode_plan_build(DecodePlan *plan, TupleDesc desc)
{
	int			i;

	dg_validate_descriptor(desc);
	plan->desc = desc;
	plan->fingerprint = descriptor_fingerprint(desc);
	plan->natts = desc->natts;
	plan->columns = MemoryContextAllocZero(CacheMemoryContext, sizeof(DecodeColumn) * Max(desc->natts, 1));
	plan->xml = false;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		DecodeColumn *column = &plan->columns[i];

		column->type = attr->atttypid;
		column->dropped = attr->attisdropped;
		if (column->dropped)
			continue;
		column->base = getBaseType(attr->atttypid);
		if (column->base == TEXTOID || column->base == BYTEAOID || column->base == PG_NODE_TREEOID)
			column->kind = DECODE_BYTES;
		else if (column->base == BOOLOID || column->base == INT2OID || column->base == INT4OID || column->base == INT8OID)
			column->kind = DECODE_INTEGER;
		else if (column->base == VARCHAROID || column->base == BPCHAROID || column->base == JSONOID)
			column->kind = DECODE_ENVELOPE_BYTES;
		else if (column->base == TIMESTAMPOID || column->base == TIMESTAMPTZOID || column->base == TIMEOID ||
				 column->base == FLOAT8OID || column->base == CASHOID)
			column->kind = DECODE_WIRE8;
		else if (column->base == DATEOID || column->base == FLOAT4OID)
			column->kind = DECODE_WIRE4;
		else if (column->base == UUIDOID)
			column->kind = DECODE_UUID;
		else if (column->base == NUMERICOID)
			column->kind = DECODE_NUMERIC;
		else if (column->base == ACLITEMOID || OidIsValid(get_element_type(column->base)) || get_typtype(column->base) == TYPTYPE_COMPOSITE)
			column->kind = DECODE_GENERAL;
		else
		{
			Oid			function;

			column->kind = DECODE_RECEIVE;
			getTypeBinaryInputInfo(column->base, &function, &column->ioparam);
			fmgr_info_cxt(function, &column->receive, CacheMemoryContext);
			plan->xml |= column->base == XMLOID;
		}
	}
}

static DecodePlan *
decode_plan(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Oid			relid = RelationGetRelid(rel);
	DecodePlan *plan;
	bool		found;

	if (!decode_plans)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DecodePlan);
		ctl.hcxt = CacheMemoryContext;
		decode_plans = hash_create("prolly decode plans", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (!decode_callback)
	{
		CacheRegisterRelcacheCallback(decode_plans_changed, (Datum) 0);
		decode_callback = true;
	}
	if (last_plan && last_plan->relid == relid && last_plan->desc == desc && last_plan->relcache && desc == rel->rd_att)
		return last_plan;
	plan = hash_search(decode_plans, &relid, HASH_ENTER, &found);

	/*
	 * The relation's own descriptor is replaced only with an invalidation,
	 * which drops the plan, so its identity suffices. A private descriptor
	 * may share an OID, and an address, with another: those are fingerprinted.
	 */
	if (found && plan->desc == desc && plan->natts == desc->natts &&
		((plan->relcache && desc == rel->rd_att) || plan->fingerprint == descriptor_fingerprint(desc)))
	{
		last_plan = plan;
		return plan;
	}
	if (found)
		pfree(plan->columns);
	plan->columns = NULL;
	PG_TRY();
	{
		decode_plan_build(plan, desc);
		plan->relcache = desc == rel->rd_att;
	}
	PG_CATCH();
	{
		hash_search(decode_plans, &relid, HASH_REMOVE, NULL);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return plan;
}

/* Decode one column's stored bytes with its planned receive function. */
/* The wire bytes inside a DGB1 envelope. */
static PLSlice
envelope_contents(PLSlice bytes)
{
	if (bytes.len < 4 || memcmp(bytes.data, "DGB1", 4))
		elog(ERROR, "invalid prolly binary codec version");
	return (PLSlice) {bytes.data + 4, bytes.len - 4};
}

/*
 * A numeric from its wire form, as numeric_recv assembles it: header words,
 * then base-10000 digits. The long format holds any value; the digits were
 * canonical when sent, so none need stripping.
 */
#define DG_NUMERIC_SPECIAL 0xC000
#define DG_NUMERIC_DSCALE_MASK 0x3FFF

static Datum
decode_numeric(PLSlice wire)
{
	int			ndigits,
				weight,
				sign,
				dscale;
	uint8	   *result;
	Size		size;

	if (wire.len < 8)
		elog(ERROR, "invalid prolly numeric encoding");
	ndigits = (int16) ((wire.data[0] << 8) | wire.data[1]);
	weight = (int16) ((wire.data[2] << 8) | wire.data[3]);
	sign = (uint16) ((wire.data[4] << 8) | wire.data[5]);
	dscale = (uint16) ((wire.data[6] << 8) | wire.data[7]);
	if (ndigits < 0 || wire.len != 8 + (Size) ndigits * 2)
		elog(ERROR, "invalid prolly numeric encoding");
	if ((sign & DG_NUMERIC_SPECIAL) == DG_NUMERIC_SPECIAL)
	{
		result = palloc(VARHDRSZ + 2);
		SET_VARSIZE(result, VARHDRSZ + 2);
		*(uint16 *) (result + VARHDRSZ) = (uint16) sign;
		return PointerGetDatum(result);
	}
	size = VARHDRSZ + 4 + (Size) ndigits * 2;
	result = palloc(size);
	SET_VARSIZE(result, size);
	*(uint16 *) (result + VARHDRSZ) = (uint16) (sign | (dscale & DG_NUMERIC_DSCALE_MASK));
	*(int16 *) (result + VARHDRSZ + 2) = (int16) weight;
	for (int i = 0; i < ndigits; i++)
		((int16 *) (result + VARHDRSZ + 4))[i] = (int16) ((wire.data[8 + 2 * i] << 8) | wire.data[9 + 2 * i]);
	return PointerGetDatum(result);
}

static Datum
receive_planned(DecodeColumn *column, PLSlice bytes)
{
	StringInfoData input;
	Datum		result;

	if (bytes.len < 4 || memcmp(bytes.data, "DGB1", 4))
		elog(ERROR, "invalid prolly binary codec version");
	bytes.data += 4;
	bytes.len -= 4;

	/* PostgreSQL 18's tsqueryrecv rejects the valid zero-item wire form. */
	if (column->base == TSQUERYOID && bytes.len == 4 && pl_read32(bytes.data) == 0)
	{
		TSQuery		query = palloc0(HDRSIZETQ);

		SET_VARSIZE(query, HDRSIZETQ);
		return PointerGetDatum(query);
	}
	initStringInfo(&input);
	appendBinaryStringInfo(&input, (const char *) bytes.data, bytes.len);
	result = ReceiveFunctionCall(&column->receive, &input, column->ioparam, -1);
	if (input.cursor != input.len)
		elog(ERROR, "trailing bytes in prolly binary value");
	pfree(input.data);
	return result;
}

/* One column's datum from its stored field, by its plan. */
static void
decode_column(DecodeColumn *column, PLValue field, Datum *value)
{
	switch (column->kind)
			{
	case DECODE_BYTES:
		*value = PointerGetDatum(cstring_to_text_with_len((const char *) field.bytes.data, field.bytes.len));
		break;
	case DECODE_INTEGER:
		{
			int64		n;

			if (field.bytes.len != 8)
				elog(ERROR, "invalid prolly integer encoding");
			n = (int64) (pl_read64(field.bytes.data) ^ UINT64CONST(0x8000000000000000));
			*value = column->base == BOOLOID ? BoolGetDatum(n != 0) :
				column->base == INT2OID ? Int16GetDatum(n) :
				column->base == INT4OID ? Int32GetDatum(n) : Int64GetDatum(n);
			break;
		}
	case DECODE_ENVELOPE_BYTES:
		{
			PLSlice		wire = envelope_contents(field.bytes);

			*value = PointerGetDatum(cstring_to_text_with_len((const char *) wire.data, wire.len));
			break;
		}
	case DECODE_WIRE8:
		{
			PLSlice		wire = envelope_contents(field.bytes);
			int64		n;

			if (wire.len != 8)
				elog(ERROR, "invalid prolly 8-byte encoding");
			n = (int64) pl_read64(wire.data);
			if (column->base == FLOAT8OID)
			{
				union
				{
					int64		i;
					float8		f;
				}			swap = {n};

				*value = Float8GetDatum(swap.f);
			}
			else
				*value = Int64GetDatum(n);
			break;
		}
	case DECODE_WIRE4:
		{
			PLSlice		wire = envelope_contents(field.bytes);
			int32		n;

			if (wire.len != 4)
				elog(ERROR, "invalid prolly 4-byte encoding");
			n = (int32) pl_read32(wire.data);
			if (column->base == FLOAT4OID)
			{
				union
				{
					int32		i;
					float4		f;
				}			swap = {n};

				*value = Float4GetDatum(swap.f);
			}
			else
				*value = DateADTGetDatum((DateADT) n);
			break;
		}
	case DECODE_UUID:
		{
			PLSlice		wire = envelope_contents(field.bytes);
			pg_uuid_t  *uuid;

			if (wire.len != UUID_LEN)
				elog(ERROR, "invalid prolly uuid encoding");
			uuid = palloc(UUID_LEN);
			memcpy(uuid->data, wire.data, UUID_LEN);
			*value = UUIDPGetDatum(uuid);
			break;
		}
	case DECODE_NUMERIC:
		*value = decode_numeric(envelope_contents(field.bytes));
		break;
	case DECODE_GENERAL:
		*value = decode_value(column->type, field.bytes);
		break;
	case DECODE_RECEIVE:
		*value = receive_planned(column, field.bytes);
		break;
			}
}

/* Whether decoding under |plan| must run with the client encoding set to the database's. */
static bool
decode_switches_encoding(DecodePlan *plan)
{
	return plan->xml || pg_get_client_encoding() != GetDatabaseEncoding();
}

/*
 * Decode columns [from, to) of |view| into |values| and |isnull| by |plan|,
 * under the database encoding when the client's differs.
 */
static void
decode_range(DecodePlan *plan, const PLTupleView *view, int from, int to, Datum *values, bool *isnull)
{
	bool		switched = decode_switches_encoding(plan);
	int			encoding = pg_get_client_encoding();
	int			saved_xmloption = xmloption;

	if (switched)
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		xmloption = XMLOPTION_CONTENT;
	}
	PG_TRY();
	{
		for (int i = from; i < to; i++)
		{
			DecodeColumn *column = &plan->columns[i];
			PLValue		field;

			dg_check(pl_tuple_field(view, i, &field));
			isnull[i] = !field.present || column->dropped;
			values[i] = (Datum) 0;
			if (!isnull[i])
				decode_column(column, field, &values[i]);
		}
	}
	PG_FINALLY();
	{
		if (switched)
		{
			xmloption = saved_xmloption;
			if (SetClientEncoding(encoding) < 0)
				elog(FATAL, "could not restore client encoding");
		}
	}
	PG_END_TRY();
}

/* ---------------------------------------------------------------------------
 * The prolly slot: a row's bytes, decoded a column at a time as asked for
 * ---------------------------------------------------------------------------
 *
 * A scan stores a row's bytes in the slot and decodes nothing. The executor
 * asks for the columns it needs, in attribute order, and only those are
 * decoded, into memory the slot owns and frees with the row. A heap slot
 * deforms its tuple the same way.
 */
typedef struct DGSlot
{
	VirtualTupleTableSlot base;
	MemoryContext row_memory;	/* decoded values; reset with each row */
	DecodePlan *plan;
	uint8	   *row;			/* the row's bytes, copied; grows as needed */
	Size		row_capacity;
	PLTupleView view;
	bool		open;			/* a row is stored and not yet fully decoded */
} DGSlot;

static void
prolly_slot_init(TupleTableSlot *slot)
{
	DGSlot	   *s = (DGSlot *) slot;

	s->row_memory = AllocSetContextCreate(slot->tts_mcxt, "prolly slot row", ALLOCSET_SMALL_SIZES);
}

static void
prolly_slot_release(TupleTableSlot *slot)
{
	DGSlot	   *s = (DGSlot *) slot;

	MemoryContextDelete(s->row_memory);
	if (s->row)
		pfree(s->row);
}

static void
prolly_slot_clear(TupleTableSlot *slot)
{
	DGSlot	   *s = (DGSlot *) slot;

	if (unlikely(TTS_SHOULDFREE(slot)))
	{
		pfree(s->base.data);
		s->base.data = NULL;
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}
	if (s->open || slot->tts_nvalid)
		MemoryContextReset(s->row_memory);
	s->open = false;
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

static void
prolly_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	DGSlot	   *s = (DGSlot *) slot;
	MemoryContext old;

	if (!s->open)
		elog(ERROR, "prolly slot holds no row to decode");
	old = MemoryContextSwitchTo(s->row_memory);
	decode_range(s->plan, &s->view, slot->tts_nvalid, Min(natts, s->plan->natts), slot->tts_values, slot->tts_isnull);
	MemoryContextSwitchTo(old);
	slot->tts_nvalid = Max(slot->tts_nvalid, Min(natts, s->plan->natts));
}

static Datum
prolly_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(!TTS_EMPTY(slot));
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));
	return 0;
}

static bool
prolly_slot_is_current_xact_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("don't have a storage tuple in this context")));
	return false;
}

/* As tts_virtual_materialize: copy every by-reference value into one allocation the slot owns. */
static void
prolly_slot_materialize(TupleTableSlot *slot)
{
	DGSlot	   *s = (DGSlot *) slot;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Size		sz = 0;
	char	   *data;

	if (TTS_SHOULDFREE(slot))
		return;
	slot_getallattrs(slot);
	for (int natt = 0; natt < desc->natts; natt++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;
		val = slot->tts_values[natt];
		if (att->attlen == -1 && VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			sz = att_nominal_alignby(sz, att->attalignby);
			sz += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			sz = att_nominal_alignby(sz, att->attalignby);
			sz = att_addlength_datum(sz, att->attlen, val);
		}
	}
	if (sz == 0)
		return;
	s->base.data = data = MemoryContextAlloc(slot->tts_mcxt, sz);
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	for (int natt = 0; natt < desc->natts; natt++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;
		val = slot->tts_values[natt];
		if (att->attlen == -1 && VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			ExpandedObjectHeader *eoh = DatumGetEOHP(val);
			Size		data_length;

			data = (char *) att_nominal_alignby(data, att->attalignby);
			data_length = EOH_get_flat_size(eoh);
			EOH_flatten_into(eoh, data, data_length);
			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
		else
		{
			Size		data_length = 0;

			data = (char *) att_nominal_alignby(data, att->attalignby);
			data_length = att_addlength_datum(data_length, att->attlen, val);
			memcpy(data, DatumGetPointer(val), data_length);
			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
	}

	/* The values no longer refer to the row's memory. */
	if (s->open)
	{
		s->open = false;
		MemoryContextReset(s->row_memory);
	}
}

static void
prolly_slot_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	TupleDesc	srcdesc = srcslot->tts_tupleDescriptor;

	prolly_slot_clear(dstslot);
	slot_getallattrs(srcslot);
	for (int natt = 0; natt < srcdesc->natts; natt++)
	{
		dstslot->tts_values[natt] = srcslot->tts_values[natt];
		dstslot->tts_isnull[natt] = srcslot->tts_isnull[natt];
	}
	dstslot->tts_nvalid = srcdesc->natts;
	dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
	prolly_slot_materialize(dstslot);
}

static HeapTuple
prolly_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));
	slot_getallattrs(slot);
	return heap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
}

static MinimalTuple
prolly_slot_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	Assert(!TTS_EMPTY(slot));
	slot_getallattrs(slot);
	return heap_form_minimal_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull, extra);
}

const TupleTableSlotOps TTSOpsProlly = {
	.base_slot_size = sizeof(DGSlot),
	.init = prolly_slot_init,
	.release = prolly_slot_release,
	.clear = prolly_slot_clear,
	.getsomeattrs = prolly_slot_getsomeattrs,
	.getsysattr = prolly_slot_getsysattr,
	.is_current_xact_tuple = prolly_slot_is_current_xact_tuple,
	.materialize = prolly_slot_materialize,
	.copyslot = prolly_slot_copyslot,
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = prolly_slot_copy_heap_tuple,
	.copy_minimal_tuple = prolly_slot_copy_minimal_tuple,
};

/*
 * Decode |bytes| into |slot| by |rel|'s plan. A prolly slot takes the bytes
 * and decodes columns as the executor asks for them; a virtual slot gets
 * every value now, in the caller's memory; any other slot a heap tuple.
 */
void
dg_decode(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	DecodePlan *plan = decode_plan(rel);
	PLTupleView view;

	if (slot->tts_ops == &TTSOpsProlly)
	{
		DGSlot	   *s = (DGSlot *) slot;

		ExecClearTuple(slot);
		if (bytes.len > s->row_capacity)
		{
			Size		capacity = Max(bytes.len, s->row_capacity * 2);

			s->row = s->row ? repalloc(s->row, capacity) : MemoryContextAlloc(slot->tts_mcxt, capacity);
			s->row_capacity = capacity;
		}
		memcpy(s->row, bytes.data, bytes.len);
		dg_check(pl_tuple_open((PLSlice) {s->row, bytes.len}, &s->view));
		if (s->view.count != (size_t) plan->natts)
			dg_unsupported("schema changes to stored tuples");
		s->plan = plan;
		s->open = true;
		slot->tts_nvalid = 0;
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		slot->tts_tid = *tid;
		slot->tts_tableOid = RelationGetRelid(rel);
		return;
	}
	if (!TTS_IS_VIRTUAL(slot))
	{
		HeapTuple	tuple = decode_fields(RelationGetDescr(rel), bytes);

		tuple->t_self = *tid;
		tuple->t_tableOid = RelationGetRelid(rel);
		ExecForceStoreHeapTuple(tuple, slot, true);
		slot->tts_tid = *tid;
		slot->tts_tableOid = RelationGetRelid(rel);
		return;
	}
	dg_check(pl_tuple_open(bytes, &view));
	if (view.count != (size_t) plan->natts)
		dg_unsupported("schema changes to stored tuples");
	ExecClearTuple(slot);
	decode_range(plan, &view, 0, plan->natts, slot->tts_values, slot->tts_isnull);
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}

HeapTuple
dg_decode_descriptor(TupleDesc desc, PLSlice bytes)
{
	dg_validate_descriptor(desc);
	return decode_fields(desc, bytes);
}

/* The canonical bytes of one value of |type|, as stored in rows. */
PLSlice
dg_encode_value(Oid type, Datum value)
{
	return encode_value(type, value);
}

Datum
dg_decode_value(Oid type, PLSlice bytes)
{
	return decode_value(type, bytes);
}

/*
 * A conversion of stored values of type |from| to type |to| and typmod
 * |typmod|, by the assignment cast ALTER TABLE ... ALTER COLUMN TYPE would
 * apply, for a merge whose other side changed a column's type.
 */
DGConversion *
dg_conversion_begin(Oid from, Oid to, int32 typmod)
{
	DGConversion *conversion = palloc0(sizeof(DGConversion));
	Param	   *param = makeNode(Param);
	Node	   *expr;

	param->paramkind = PARAM_EXTERN;
	param->paramid = 1;
	param->paramtype = from;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(from);
	param->location = -1;
	expr = coerce_to_target_type(NULL, (Node *) param, from, to, typmod,
								 COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
	if (!expr)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot merge values of type %s into a column of type %s",
						format_type_be(from), format_type_with_typemod(to, typmod))));
	conversion->from = from;
	conversion->to = to;
	conversion->params = makeParamList(1);
	conversion->params->params[0].ptype = from;
	conversion->params->params[0].pflags = PARAM_FLAG_CONST;
	conversion->params->numParams = 1;
	conversion->state = ExecInitExprWithParams((Expr *) expression_planner((Expr *) expr), conversion->params);
	conversion->context = CreateStandaloneExprContext();
	conversion->context->ecxt_param_list_info = conversion->params;
	return conversion;
}

PLSlice
dg_conversion_apply(DGConversion *conversion, PLSlice bytes)
{
	bool		isnull;
	Datum		value;

	conversion->params->params[0].value = decode_value(conversion->from, bytes);
	conversion->params->params[0].isnull = false;
	value = ExecEvalExprSwitchContext(conversion->state, conversion->context, &isnull);
	if (isnull)
		elog(ERROR, "a merged value converted to NULL");
	return encode_value(conversion->to, value);
}

/*
 * A change to one user type that stored values must follow: an enum label
 * renamed, or a composite type's fields laid out again.
 */
typedef struct Recode
{
	Oid			target;
	const char *old;			/* enum label, and its new name */
	const char *renamed;
	int			count;			/* composite fields: the old field of each */
	const int  *mapping;		/* new one, or -1 for a new, absent field */
} Recode;

static PLSlice
recode_value(Oid type, PLSlice bytes, const Recode *recode)
{
	Oid			element;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID || type == BOOLOID || type == INT2OID ||
		type == INT4OID || type == INT8OID)
		return bytes;
	element = get_element_type(type);
	if (OidIsValid(element) || get_typtype(type) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = OidIsValid(element) ? NULL : lookup_rowtype_tupdesc(type, -1);
		uint32		count;
		PLValue    *fields;
		PLSlice		result;

		if (bytes.len < 12)
			elog(ERROR, "invalid prolly nested value");
		count = pl_read32(bytes.data + 8);
		fields = palloc0_array(PLValue, Max(count, 1));
		for (uint32 i = 0; i < count; i++)
			dg_check(pl_tuple_get(bytes, i, &fields[i]));
		if (type == recode->target && recode->mapping)
		{
			/* The old fields, in their new places; |desc| is the new layout. */
			PLValue    *moved = palloc0_array(PLValue, Max(recode->count, 1));

			for (int i = 0; i < recode->count; i++)
				if (recode->mapping[i] >= 0 && recode->mapping[i] < count)
					moved[i] = fields[recode->mapping[i]];
			fields = moved;
			count = recode->count;
		}
		for (uint32 i = 0; i < count; i++)
		{
			Oid			field;

			if (!fields[i].present)
				continue;
			/* An array's first field is its shape. */
			if (OidIsValid(element))
			{
				if (i == 0)
					continue;
				field = element;
			}
			else
				field = TupleDescAttr(desc, i)->atttypid;
			fields[i].bytes = recode_value(field, fields[i].bytes, recode);
		}
		result = pack_fields(fields, count);
		if (desc)
			ReleaseTupleDesc(desc);
		pfree(fields);
		return result;
	}
	if (type == recode->target && recode->old &&
		bytes.len == 4 + strlen(recode->old) && !memcmp(bytes.data + 4, recode->old, strlen(recode->old)))
	{
		uint8	   *data = palloc(4 + strlen(recode->renamed));

		memcpy(data, "DGB1", 4);
		memcpy(data + 4, recode->renamed, strlen(recode->renamed));
		return (PLSlice)
		{
			data, 4 + strlen(recode->renamed)
		};
	}
	return bytes;
}

/*
 * The canonical bytes of one value of |type| with every label |old| of enum
 * |target| replaced by |renamed|, as when the label is renamed. Stored enum
 * values are their labels; the structure of every type is unchanged.
 */
PLSlice
dg_recode_label(Oid type, PLSlice bytes, Oid target, const char *old, const char *renamed)
{
	Recode		recode = {
		.target = target,
		.old = old,
		.renamed = renamed,
	};

	return recode_value(type, bytes, &recode);
}

/*
 * The canonical bytes of one value of |type| with every value of composite
 * type |target| holding its fields as |target| now lays them out: new field i
 * is old field |mapping|[i], or absent if that is -1. Stored composite values
 * hold their fields by position, so a type's added or dropped attributes
 * change them.
 */
PLSlice
dg_recode_composite(Oid type, PLSlice bytes, Oid target, int count, const int *mapping)
{
	Recode		recode = {
		.target = target,
		.count = count,
		.mapping = mapping,
	};

	return recode_value(type, bytes, &recode);
}

/*
 * Index keys. A key column is its value's bytes in an order matching the
 * type's btree operator class, with values the class finds equal (such as
 * numeric 1.0 and 1.00, or float -0 and 0) encoded the same. For the types
 * whose row encoding already has these properties, it is the row encoding.
 */
static bool
key_is_row(Oid type)
{
	return type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID ||
		type == TEXTOID || type == BYTEAOID;
}

static PLSlice
key_bytes(const void *data, size_t len)
{
	uint8	   *copy = palloc(Max(len, 1));

	memcpy(copy, data, len);
	return (PLSlice)
	{
		copy, len
	};
}

static PLSlice
key_int64(int64 n)
{
	uint8	   *data = palloc(8);

	pl_key_int64(n, data);
	return (PLSlice)
	{
		data, 8
	};
}

/* IEEE order, with -0 as 0 and every NaN above +Infinity, as float8 sorts. */
static PLSlice
key_float(double value)
{
	uint64		bits;
	uint8	   *data = palloc(8);

	if (value == 0)
		value = 0;
	if (isnan(value))
		bits = UINT64CONST(0x7FF8000000000000);
	else
		memcpy(&bits, &value, 8);
	bits = (bits & UINT64CONST(0x8000000000000000)) ? ~bits : bits | UINT64CONST(0x8000000000000000);
	pl_write64(data, bits);
	return (PLSlice)
	{
		data, 8
	};
}

/*
 * A numeric as a tag (-Infinity, negative, zero, positive, +Infinity, NaN),
 * then for a finite nonzero value its decimal exponent and significant
 * digits, trailing zeros removed. A negative value's exponent and digits are
 * complemented and end with 0xFF, so a longer magnitude sorts first.
 */
static PLSlice
key_numeric(Numeric value)
{
	char	   *text = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(value)));
	StringInfoData out;
	bool		negative = text[0] == '-';
	char	   *digits = palloc(strlen(text) + 1);
	int			n = 0,
				integer = -1,
				exponent;
	uint8		e[4];

	initStringInfo(&out);
	if (!strcmp(text, "NaN") || !strcmp(text, "Infinity") || !strcmp(text, "-Infinity"))
	{
		appendStringInfoChar(&out, text[0] == 'N' ? 6 : negative ? 1 : 5);
		return (PLSlice)
		{
			(uint8 *) out.data, out.len
		};
	}
	for (char *c = text + (negative ? 1 : 0); *c; c++)
	{
		if (*c == '.')
			integer = n;
		else
			digits[n++] = *c;
	}
	if (integer < 0)
		integer = n;
	exponent = integer;
	while (n > 0 && digits[0] == '0')
	{
		memmove(digits, digits + 1, --n);
		exponent--;
	}
	while (n > 0 && digits[n - 1] == '0')
		n--;
	if (n == 0)
	{
		appendStringInfoChar(&out, 3);
		return (PLSlice)
		{
			(uint8 *) out.data, out.len
		};
	}
	appendStringInfoChar(&out, negative ? 2 : 4);
	pl_write32(e, (uint32) exponent ^ 0x80000000);
	for (int i = 0; i < 4; i++)
		appendStringInfoChar(&out, negative ? ~e[i] : e[i]);
	for (int i = 0; i < n; i++)
		appendStringInfoChar(&out, negative ? ~digits[i] : digits[i]);
	if (negative)
		appendStringInfoChar(&out, (char) 0xFF);
	return (PLSlice)
	{
		(uint8 *) out.data, out.len
	};
}

/* An interval by the 128-bit span interval_cmp compares. */
static PLSlice
key_interval(Interval *interval)
{
	INT128		span = int64_to_int128(interval->time);
	uint8	   *data = palloc(16);
	int64		hi;
	uint64		lo;

	int128_add_int64_mul_int64(&span, interval->month * INT64CONST(30) + interval->day, USECS_PER_DAY);
#ifdef USE_NATIVE_INT128
	hi = (int64) (span >> 64);
	lo = (uint64) span;
#else
	hi = span.hi;
	lo = span.lo;
#endif
	pl_write64(data, (uint64) hi ^ UINT64CONST(0x8000000000000000));
	pl_write64(data + 8, lo);
	return (PLSlice)
	{
		data, 16
	};
}

/* Whether a prolly index can order keys of |type| as its operator class does. */
bool
dg_key_ordered(Oid type)
{
	Oid			element;
	char		kind;

	check_stack_depth();
	type = getBaseType(type);
	element = get_element_type(type);
	if (OidIsValid(element))
		return dg_key_ordered(element);
	kind = get_typtype(type);
	if (kind == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
		bool		supported = true;

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute a = TupleDescAttr(desc, i);

			if (!a->attisdropped &&
				(!dg_key_ordered(a->atttypid) ||
				 (OidIsValid(a->attcollation) && !pg_newlocale_from_collation(a->attcollation)->collate_is_c)))
				supported = false;
		}
		ReleaseTupleDesc(desc);
		return supported;
	}
	if (kind == TYPTYPE_MULTIRANGE)
		return dg_key_ordered(get_multirange_range(type));
	if (kind == TYPTYPE_RANGE)
	{
		TypeCacheEntry *range = lookup_type_cache(type, TYPECACHE_RANGE_INFO);
		TypeCacheEntry *subtype = lookup_type_cache(range->rngelemtype->type_id, TYPECACHE_CMP_PROC);

		/* A custom subtype ordering cannot use the default type's key bytes. */
		return range->rng_cmp_proc_finfo.fn_oid == subtype->cmp_proc &&
			(!OidIsValid(range->rng_collation) || pg_newlocale_from_collation(range->rng_collation)->collate_is_c) &&
			dg_key_ordered(subtype->type_id);
	}
	switch (type)
	{
		case JSONBOID:
			/* jsonb compares strings using the database default collation. */
			return pg_newlocale_from_collation(DEFAULT_COLLATION_OID)->collate_is_c;
		case OIDOID:
		case BITOID:
		case VARBITOID:
		case INETOID:
		case CIDROID:
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case TEXTOID:
		case BYTEAOID:
		case VARCHAROID:
		case BPCHAROID:
		case CHAROID:
		case NAMEOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case INTERVALOID:
		case UUIDOID:
		case CASHOID:
		case MACADDROID:
		case MACADDR8OID:
			return true;
		default:
			return kind == TYPTYPE_ENUM;
	}
}

/*
 * Match array_cmp: compare flattened elements, then dimensionality, dimensions,
 * and lower bounds. End-of-array sorts before any next element; an element NULL
 * sorts after every non-NULL. Escaping makes variable-width elements unambiguous.
 */
static PLSlice
key_array(Datum value)
{
	ArrayType  *array = DatumGetArrayTypeP(value);
	Oid			element = ARR_ELEMTYPE(array);
	Datum	   *values;
	bool	   *nulls;
	int			count;
	int16		length;
	bool		byval;
	char		align;
	StringInfoData out;

	get_typlenbyvalalign(element, &length, &byval, &align);
	deconstruct_array(array, element, length, byval, align, &values, &nulls, &count);
	initStringInfo(&out);
	for (int i = 0; i < count; i++)
	{
		appendStringInfoChar(&out, nulls[i] ? 2 : 1);
		if (!nulls[i])
		{
			PLSlice		key = dg_key_value(element, values[i]);

			for (size_t j = 0; j < key.len; j++)
			{
				appendStringInfoChar(&out, key.data[j]);
				if (!key.data[j])
					appendStringInfoChar(&out, 255);
			}
			appendStringInfoChar(&out, 0);
			appendStringInfoChar(&out, 0);
			/* Text/bytea keys borrow their input's payload. */
			if (getBaseType(element) != TEXTOID && getBaseType(element) != BYTEAOID)
				pfree((void *) key.data);
		}
	}
	appendStringInfoChar(&out, 0);
	{
		PLSlice		key = key_int64(ARR_NDIM(array));

		appendBinaryStringInfo(&out, (char *) key.data, key.len);
		pfree((void *) key.data);
	}
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < ARR_NDIM(array); i++)
		{
			PLSlice		key = key_int64(pass ? ARR_LBOUND(array)[i] : ARR_DIMS(array)[i]);

			appendBinaryStringInfo(&out, (char *) key.data, key.len);
			pfree((void *) key.data);
		}
	pfree(values);
	pfree(nulls);
	if ((Pointer) array != DatumGetPointer(value))
		pfree(array);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

/* Escaped, terminated components preserve ordering and delimit nested keys. */
static void
key_component(StringInfo out, PLSlice key)
{
	for (size_t i = 0; i < key.len; i++)
	{
		appendStringInfoChar(out, key.data[i]);
		if (!key.data[i])
			appendStringInfoChar(out, 255);
	}
	appendStringInfoChar(out, 0);
	appendStringInfoChar(out, 0);
}

/* bitcmp compares padded bytes, then bit length. Mask the final byte so this
 * also encodes network prefixes without including their host bits. Escaping
 * terminates the byte string before the length, retaining prefix ordering. */
static void
key_bits(StringInfo out, const unsigned char *data, int bits)
{
	int         bytes = (bits + 7) / 8;
	uint8       length[4];

	for (int i = 0; i < bytes; i++)
	{
		uint8 byte = data[i];

		if (i == bytes - 1 && bits % 8)
			byte &= 0xff << (8 - bits % 8);
		appendStringInfoChar(out, byte);
		if (!byte)
			appendStringInfoChar(out, 255);
	}
	appendStringInfoChar(out, 0);
	appendStringInfoChar(out, 0);
	pl_write32(length, bits);
	appendBinaryStringInfo(out, (char *) length, sizeof(length));
}

static PLSlice
key_network(Datum value)
{
	inet	   *ip = DatumGetInetPP(value);
	StringInfoData out;

	initStringInfo(&out);
	appendStringInfoChar(&out, ip_family(ip) == PGSQL_AF_INET ? 1 : 2);
	key_bits(&out, ip_addr(ip), ip_bits(ip));
	appendBinaryStringInfo(&out, (char *) ip_addr(ip), ip_addrsize(ip));
	if ((Pointer) ip != DatumGetPointer(value))
		pfree(ip);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

static void
key_bound(StringInfo out, TypeCacheEntry *range, RangeBound *bound)
{
	appendStringInfoChar(out, bound->infinite ? (bound->lower ? 0 : 2) : 1);
	if (!bound->infinite)
	{
		key_component(out, dg_key_value(range->rngelemtype->type_id, bound->val));
		appendStringInfoChar(out, bound->lower ? !bound->inclusive : bound->inclusive);
	}
}

static PLSlice
key_range(Datum value)
{
	RangeType  *r = DatumGetRangeTypeP(value);
	TypeCacheEntry *type = lookup_type_cache(RangeTypeGetOid(r), TYPECACHE_RANGE_INFO);
	RangeBound	lower,
				upper;
	bool		empty;
	StringInfoData out;

	initStringInfo(&out);
	range_deserialize(type, r, &lower, &upper, &empty);
	appendStringInfoChar(&out, empty ? 0 : 1);
	if (!empty)
	{
		key_bound(&out, type, &lower);
		key_bound(&out, type, &upper);
	}
	if ((Pointer) r != DatumGetPointer(value))
		pfree(r);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

static PLSlice
key_multirange(Datum value)
{
	MultirangeType *r = DatumGetMultirangeTypeP(value);
	TypeCacheEntry *type = lookup_type_cache(MultirangeTypeGetOid(r), TYPECACHE_MULTIRANGE_INFO);
	StringInfoData out;

	initStringInfo(&out);
	for (uint32 i = 0; i < r->rangeCount; i++)
	{
		RangeBound	lower,
				upper;

		appendStringInfoChar(&out, 1);
		multirange_get_bounds(type->rngtype, r, i, &lower, &upper);
		key_bound(&out, type->rngtype, &lower);
		key_bound(&out, type->rngtype, &upper);
	}
	appendStringInfoChar(&out, 0);
	if ((Pointer) r != DatumGetPointer(value))
		pfree(r);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

static PLSlice
key_record(Datum value)
{
	HeapTupleHeader header = DatumGetHeapTupleHeader(value);
	TupleDesc	desc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(header), HeapTupleHeaderGetTypMod(header));
	HeapTupleData tuple;
	Datum	   *values = palloc_array(Datum, desc->natts);
	bool	   *nulls = palloc_array(bool, desc->natts);
	StringInfoData out;

	tuple.t_len = HeapTupleHeaderGetDatumLength(header);
	tuple.t_data = header;
	heap_deform_tuple(&tuple, desc, values, nulls);
	initStringInfo(&out);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(desc, i);

		if (a->attisdropped)
			continue;
		appendStringInfoChar(&out, nulls[i] ? 2 : 1);
		if (!nulls[i])
			key_component(&out, dg_key_value(a->atttypid, values[i]));
	}
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);
	if ((Pointer) header != DatumGetPointer(value))
		pfree(header);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

/* Mirror compareJsonbContainers, including its top-level raw-scalar array
 * ordering anomaly: [] sorts below null, then scalars, then nonempty arrays. */
static PLSlice
key_jsonb(Datum value)
{
	Jsonb	   *json = DatumGetJsonbP(value);
	JsonbIterator *it = JsonbIteratorInit(&json->root);
	JsonbValue	v;
	JsonbIteratorToken token;
	StringInfoData out;
	bool		root = true;

	initStringInfo(&out);
	while ((token = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (token == WJB_END_ARRAY || token == WJB_END_OBJECT)
			continue;
		appendStringInfoChar(&out, v.type);
		switch (v.type)
		{
			case jbvArray:
				key_component(&out, key_int64(v.val.array.nElems));
				if (root)
					appendStringInfoChar(&out, v.val.array.rawScalar ? 0 : 1);
				break;
			case jbvObject:
				key_component(&out, key_int64(v.val.object.nPairs));
				break;
			case jbvString:
				key_component(&out, (PLSlice) {(uint8 *) v.val.string.val, v.val.string.len});
				break;
			case jbvNumeric:
				key_component(&out, key_numeric(v.val.numeric));
				break;
			case jbvBool:
				appendStringInfoChar(&out, v.val.boolean);
				break;
			case jbvNull:
				break;
			default:
				elog(ERROR, "unexpected jsonb key token");
		}
		root = false;
	}
	if ((Pointer) json != DatumGetPointer(value))
		pfree(json);
	return (PLSlice) {(uint8 *) out.data, out.len};
}

/* The key bytes of |value| of |type|. */
PLSlice
dg_key_value(Oid type, Datum value)
{
	check_stack_depth();
	type = getBaseType(type);
	if (OidIsValid(get_element_type(type)))
		return key_array(value);
	switch (get_typtype(type))
	{
		case TYPTYPE_COMPOSITE:
			return key_record(value);
		case TYPTYPE_RANGE:
			return key_range(value);
		case TYPTYPE_MULTIRANGE:
			return key_multirange(value);
	}
	if (key_is_row(type))
		return encode_value(type, value);
	switch (type)
	{
		case OIDOID:
			return key_int64((int64) DatumGetObjectId(value));
		case INETOID:
		case CIDROID:
			return key_network(value);
		case JSONBOID:
			return key_jsonb(value);
		case BITOID:
		case VARBITOID:
			{
				VarBit	   *bits = DatumGetVarBitP(value);
				StringInfoData out;

				initStringInfo(&out);
				key_bits(&out, VARBITS(bits), VARBITLEN(bits));
				if ((Pointer) bits != DatumGetPointer(value))
					pfree(bits);
				return (PLSlice) {(uint8 *) out.data, out.len};
			}
		case VARCHAROID:
			{
				struct varlena *v = PG_DETOAST_DATUM_PACKED(value);

				return key_bytes(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
			}
		case BPCHAROID:
			{
				/* bpchar compares without its trailing spaces. */
				struct varlena *v = PG_DETOAST_DATUM_PACKED(value);
				size_t		len = VARSIZE_ANY_EXHDR(v);

				while (len > 0 && VARDATA_ANY(v)[len - 1] == ' ')
					len--;
				return key_bytes(VARDATA_ANY(v), len);
			}
		case NAMEOID:
			return key_bytes(NameStr(*DatumGetName(value)), strlen(NameStr(*DatumGetName(value))));
		case CHAROID:
			{
				uint8		c = (uint8) DatumGetChar(value);

				return key_bytes(&c, 1);
			}
		case FLOAT4OID:
			return key_float(DatumGetFloat4(value));
		case FLOAT8OID:
			return key_float(DatumGetFloat8(value));
		case NUMERICOID:
			return key_numeric(DatumGetNumeric(value));
		case DATEOID:
			return key_int64(DatumGetDateADT(value));
		case TIMEOID:
			return key_int64(DatumGetTimeADT(value));
		case TIMESTAMPOID:
			return key_int64(DatumGetTimestamp(value));
		case TIMESTAMPTZOID:
			return key_int64(DatumGetTimestampTz(value));
		case CASHOID:
			return key_int64(DatumGetCash(value));
		case TIMETZOID:
			{
				/* As timetz_cmp: UTC time, then zone. */
				TimeTzADT  *time = DatumGetTimeTzADTP(value);
				PLSlice		when = key_int64(time->time + time->zone * USECS_PER_SEC);
				PLSlice		zone = key_int64(time->zone);
				uint8	   *data = palloc(16);

				memcpy(data, when.data, 8);
				memcpy(data + 8, zone.data, 8);
				return (PLSlice)
				{
					data, 16
				};
			}
		case INTERVALOID:
			return key_interval(DatumGetIntervalP(value));
		case UUIDOID:
			return key_bytes(DatumGetUUIDP(value)->data, UUID_LEN);
		case MACADDROID:
			return key_bytes(DatumGetMacaddrP(value), sizeof(macaddr));
		case MACADDR8OID:
			return key_bytes(DatumGetMacaddr8P(value), sizeof(macaddr8));
	}
	if (get_typtype(type) == TYPTYPE_ENUM)
	{
		/* A label by its position in its enum. */
		HeapTuple	tuple = SearchSysCache1(ENUMOID, value);
		PLSlice		result;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for enum label %u", DatumGetObjectId(value));
		result = key_float(((Form_pg_enum) GETSTRUCT(tuple))->enumsortorder);
		ReleaseSysCache(tuple);
		return result;
	}
	dg_unsupported("ordered index keys of this type");
	pg_unreachable();
}

/* The key bytes of a value stored in a row as |bytes|. */
PLSlice
dg_key_field(Oid type, PLSlice bytes)
{
	type = getBaseType(type);
	if (key_is_row(type))
		return bytes;
	return dg_key_value(type, decode_value(type, bytes));
}
