/* Portable pg_proc values. Local object identifiers never enter schema bytes. */

/* A type's portable name: qualified, with [] for an array. */
static char *
routine_type_name(Oid id)
{
	HeapTuple	tuple;
	Form_pg_type type;
	char	   *result;
	Oid			element;

	if (!OidIsValid(id))
		return pstrdup("");
	element = get_element_type(id);
	/* Vectors are distinct named types, not aliases for ordinary arrays. */
	if (OidIsValid(element) && get_array_type(element) == id)
		return psprintf("%s[]", routine_type_name(element));
	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(id));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", id);
	type = (Form_pg_type) GETSTRUCT(tuple);
	/* A user type is portable once versioned: a table's row type or a type of its own. */
	if (type->typnamespace != PG_CATALOG_NAMESPACE && id < FirstDurableObjectId)
		unsupported("versioned object referring to an unversioned type");
	result = dg_qualified_name(get_namespace_name(type->typnamespace), NameStr(type->typname));
	ReleaseSysCache(tuple);
	return result;
}

static char *
routine_encode(HeapTuple tuple, TupleDesc desc)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tuple);
	StringInfoData json,
				identity;
	char	   *dependencies = NULL;
	HeapTuple	language = SearchSysCache1(LANGOID, ObjectIdGetDatum(proc->prolang));
	char	   *lang = pstrdup(NameStr(((Form_pg_language) GETSTRUCT(language))->lanname));
	int			i;

	ReleaseSysCache(language);
	if (proc->prokind != PROKIND_AGGREGATE && strcmp(lang, "sql") && strcmp(lang, "plpgsql"))
		unsupported("canonical routines require SQL or PL/pgSQL");
	if ((proc->prokind != PROKIND_FUNCTION && proc->prokind != PROKIND_PROCEDURE && proc->prokind != PROKIND_AGGREGATE) || proc->prosupport)
		unsupported("canonical routine kind or support function");
	initStringInfo(&identity);
	appendStringInfo(&identity, "%s(", dg_qualified_name(get_namespace_name(proc->pronamespace), NameStr(proc->proname)));
	for (i = 0; i < proc->pronargs; i++)
		appendStringInfo(&identity, "%s%s", i ? "," : "", routine_type_name(proc->proargtypes.values[i]));
	appendStringInfoChar(&identity, ')');
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGROUTINE1\",");
	json_field(&json, "name", identity.data, false);
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		bool		isnull;
		Datum		value = heap_getattr(tuple, i + 1, desc, &isnull);
		char	   *encoded;
		Oid			output;
		bool		varlena;

		if (i + 1 == Anum_pg_proc_oid)
			continue;
		appendStringInfoChar(&json, ',');
		escape_json(&json, NameStr(attr->attname));
		appendStringInfoChar(&json, ':');
		if (isnull)
		{
			appendStringInfoString(&json, "null");
			continue;
		}
		switch (i + 1)
		{
			case Anum_pg_proc_pronamespace:
				encoded = get_namespace_name(DatumGetObjectId(value));
				break;
			case Anum_pg_proc_proowner:
				encoded = GetUserNameFromId(DatumGetObjectId(value), false);
				break;
			case Anum_pg_proc_prolang:
				encoded = lang;
				break;
			case Anum_pg_proc_prorettype:
			case Anum_pg_proc_provariadic:
				encoded = routine_type_name(DatumGetObjectId(value));
				break;
			case Anum_pg_proc_proargtypes:
			case Anum_pg_proc_proallargtypes:
				{
					Oid		   *ids;
					int			n;
					Datum	   *names;

					if (i + 1 == Anum_pg_proc_proargtypes)
					{
						oidvector  *vector = (oidvector *) DatumGetPointer(value);

						ids = vector->values;
						n = vector->dim1;
					}
					else
					{
						ArrayType  *array = DatumGetArrayTypeP(value);

						n = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
						ids = (Oid *) ARR_DATA_PTR(array);
					}
					names = palloc_array(Datum, Max(n, 1));
					for (int j = 0; j < n; j++)
						names[j] = CStringGetTextDatum(routine_type_name(ids[j]));
					encoded = OidOutputFunctionCall(F_ARRAY_OUT, PointerGetDatum(construct_array(names, n, TEXTOID, -1, false, TYPALIGN_INT)));
					break;
				}
			case Anum_pg_proc_proargdefaults:
				{
					/* Defaults are portable SQL, rebound when the view is built. */
					List	   *defaults = castNode(List, stringToNode(TextDatumGetCString(value)));
					Datum	   *sql = palloc_array(Datum, Max(list_length(defaults), 1));
					int			n = 0;

					foreach_ptr(Node, expression, defaults)
						sql[n++] = CStringGetTextDatum(schema_expression_sql(expression, InvalidOid));
					encoded = OidOutputFunctionCall(F_ARRAY_OUT, PointerGetDatum(construct_array(sql, n, TEXTOID, -1, false, TYPALIGN_INT)));
					break;
				}
			case Anum_pg_proc_prosqlbody:
				/* A SQL-standard body is its SQL, analyzed again when read. */
				encoded = routine_body_encode(tuple, &dependencies);
				break;
			case Anum_pg_proc_protrftypes:
				unsupported("canonical routine transforms");
				pg_unreachable();
			default:
				getTypeOutputInfo(attr->atttypid, &output, &varlena);
				encoded = OidOutputFunctionCall(output, value);
				break;
		}
		escape_json(&json, encoded);
	}
	if (dependencies)
		appendStringInfo(&json, ",\"dependencies\":%s", dependencies);
	json_comment(&json, proc->oid, ProcedureRelationId);
	appendStringInfoChar(&json, '}');
	run("SELECT $1::jsonb::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json.data)), NULL, true);
	return strcell(0, 1);
}

/*
 * A schema object stored apart from tables, such as a routine or trigger: its
 * member payload names it and hashes its definition, held in a child chunk.
 */
static char *
object_payload(const char *kind, const char *definition)
{
	/* The child schema retains an allocator seed for restore. Its logical
	 * fingerprint versions the definition, not the shared counter snapshot. */
	if (!strcmp(kind, "sequence"))
	{
		run("SELECT ($1::jsonb - 'last_value' - 'is_called')::text", 1,
			VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		definition = strcell(0, 1);
	}
	run("SELECT jsonb_build_object('kind',$2::text,'name',$1::jsonb->>'name','schema',encode(sha256(convert_to($1,'UTF8')),'hex'))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(kind)), NULL, true);
	return strcell(0, 1);
}

static bytea *
object_member(const char *kind, const char *definition)
{
	bytea	   *schema = put("schema", definition, NULL, 0);

	return put(kind, object_payload(kind, definition), VC_ARGS(PointerGetDatum(schema)), 1);
}
