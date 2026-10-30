/* Portable native aggregate/operator/cast catalog tuples. Every incidental
 * object identifier is replaced by a qualified name or full signature. */
static Oid
schema_object_class(const char *kind)
{
	if (!strcmp(kind, "aggregate"))
		return ProcedureRelationId;
	if (!strcmp(kind, "operator"))
		return OperatorRelationId;
	if (!strcmp(kind, "cast"))
		return CastRelationId;
	return InvalidOid;
}

static char *
schema_proc_name(Oid id)
{
	HeapTuple tuple;
	Form_pg_proc proc;
	StringInfoData name;

	if (!OidIsValid(id))
		return pstrdup("");
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(id));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "missing schema routine %u", id);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	initStringInfo(&name);
	appendStringInfo(&name, "%s(", dg_qualified_name(get_namespace_name(proc->pronamespace), NameStr(proc->proname)));
	for (int i = 0; i < proc->pronargs; i++)
		appendStringInfo(&name, "%s%s", i ? "," : "", routine_type_name(proc->proargtypes.values[i]));
	appendStringInfoChar(&name, ')');
	ReleaseSysCache(tuple);
	return name.data;
}

static char *
schema_operator_name(Oid id)
{
	HeapTuple tuple;
	Form_pg_operator op;
	char *name;

	if (!OidIsValid(id))
		return pstrdup("");
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(id));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "missing schema operator %u", id);
	op = (Form_pg_operator) GETSTRUCT(tuple);
	name = psprintf("%s.%s(%s,%s)", quote_identifier(get_namespace_name(op->oprnamespace)), NameStr(op->oprname),
		op->oprleft ? routine_type_name(op->oprleft) : "NONE", routine_type_name(op->oprright));
	ReleaseSysCache(tuple);
	return name;
}

/* Referenced catalog for each OID-bearing column, zero for scalar values. */
static Oid
schema_field_class(Oid catalog, const char *field)
{
	if (!strcmp(field, "oid"))
		return catalog;
	if (catalog == AggregateRelationId)
	{
		if (!strcmp(field, "aggtranstype") || !strcmp(field, "aggmtranstype"))
			return TypeRelationId;
		if (!strcmp(field, "aggsortop"))
			return OperatorRelationId;
		if (!strcmp(field, "aggfnoid") || strstr(field, "fn"))
			return ProcedureRelationId;
	}
	if (catalog == OperatorRelationId)
	{
		if (!strcmp(field, "oprnamespace")) return NamespaceRelationId;
		if (!strcmp(field, "oprowner")) return AuthIdRelationId;
		if (!strcmp(field, "oprleft") || !strcmp(field, "oprright") || !strcmp(field, "oprresult")) return TypeRelationId;
		if (!strcmp(field, "oprcom") || !strcmp(field, "oprnegate")) return OperatorRelationId;
		if (!strcmp(field, "oprcode") || !strcmp(field, "oprrest") || !strcmp(field, "oprjoin")) return ProcedureRelationId;
	}
	if (catalog == CastRelationId)
	{
		if (!strcmp(field, "castsource") || !strcmp(field, "casttarget")) return TypeRelationId;
		if (!strcmp(field, "castfunc")) return ProcedureRelationId;
	}
	return InvalidOid;
}

static char *
schema_object_encode(Oid catalog, HeapTuple tuple, TupleDesc desc)
{
	StringInfoData json;
	StringInfoData dependencies;
	Oid id;
	Oid objectclass = catalog == AggregateRelationId ? ProcedureRelationId : catalog;
	char *name;
	char *routine = NULL;

	if (catalog == AggregateRelationId)
	{
		HeapTuple proc;
		Relation rel;

		id = ((Form_pg_aggregate) GETSTRUCT(tuple))->aggfnoid;
		name = schema_proc_name(id);
		proc = SearchSysCache1(PROCOID, ObjectIdGetDatum(id));
		rel = table_open(ProcedureRelationId, AccessShareLock);
		routine = routine_encode(proc, RelationGetDescr(rel));
		table_close(rel, AccessShareLock);
		ReleaseSysCache(proc);
	}
	else if (catalog == OperatorRelationId)
	{
		id = ((Form_pg_operator) GETSTRUCT(tuple))->oid;
		name = schema_operator_name(id);
	}
	else
	{
		Form_pg_cast cast = (Form_pg_cast) GETSTRUCT(tuple);

		id = cast->oid;
		name = psprintf("(%s AS %s)", routine_type_name(cast->castsource), routine_type_name(cast->casttarget));
	}
	CatalogViewNameObject(id, name);
	initStringInfo(&json);
	initStringInfo(&dependencies);
	appendStringInfoString(&dependencies, "[");
	appendStringInfoString(&json, "{\"format\":\"DGSCHEMAOBJECT1\",");
	json_field(&json, "name", name, false);
	appendStringInfoString(&json, ",\"row\":{");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		char *field = NameStr(attr->attname);
		bool missing;
		Datum value = heap_getattr(tuple, i + 1, desc, &missing);
		Oid reference = schema_field_class(catalog, field);
		char *encoded;

		if (!strcmp(field, "oid"))
			continue;
		if (json.data[json.len - 1] != '{')
			appendStringInfoChar(&json, ',');
		escape_json(&json, field);
		appendStringInfoChar(&json, ':');
		if (missing)
		{
			appendStringInfoString(&json, "null");
			continue;
		}
		if (reference)
		{
			Oid referenced = DatumGetObjectId(value);

			encoded = !referenced ? pstrdup("") :
				reference == TypeRelationId ? routine_type_name(referenced) :
				reference == ProcedureRelationId ? schema_proc_name(referenced) :
				reference == OperatorRelationId ? schema_operator_name(referenced) :
				reference == NamespaceRelationId ? get_namespace_name(referenced) : GetUserNameFromId(referenced, false);
			if (referenced >= FirstDurableObjectId && referenced != id && reference != AuthIdRelationId)
			{
				if (dependencies.len > 1) appendStringInfoChar(&dependencies, ',');
				appendStringInfoChar(&dependencies, '[');
				escape_json(&dependencies, reference == TypeRelationId ? "type" : reference == OperatorRelationId ? "operator" : reference == NamespaceRelationId ? "namespace" : "routine");
				appendStringInfoChar(&dependencies, ',');
				escape_json(&dependencies, encoded);
				appendStringInfoChar(&dependencies, ']');
			}
		}
		else
		{
			Oid output;
			bool varlena;

			if (attr->atttypid == OIDOID || attr->atttypid == REGPROCOID)
				elog(ERROR, "unclassified schema identifier: %s", field);
			getTypeOutputInfo(attr->atttypid, &output, &varlena);
			encoded = OidOutputFunctionCall(output, value);
		}
		escape_json(&json, encoded);
	}
	appendStringInfo(&json, "},\"dependencies\":%s]", dependencies.data);
	if (routine)
		appendStringInfo(&json, ",\"routine\":%s", routine);
	json_comment(&json, id, objectclass);
	appendStringInfoChar(&json, '}');
	run("SELECT $1::jsonb::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json.data)), NULL, true);
	return strcell(0, 1);
}
