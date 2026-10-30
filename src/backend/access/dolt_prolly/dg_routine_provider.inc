static Oid provider_array_of(DGProviderRows *rows, Oid element);

/* Resolve symbolic signatures against the view under construction. */
/* The type an installed extension names |name|, or InvalidOid. */
static Oid
extension_type_oid(const char *name)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	Oid			result = InvalidOid;
	int32		typmod;

	PG_TRY();
	{
		if (parseTypeString(name, &result, &typmod, (Node *) &(ErrorSaveContext) {T_ErrorSaveContext}) &&
			OidIsValid(result) && !schema_extension_member(TypeRelationId, result))
			result = InvalidOid;
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static Oid
routine_type_oid(DGProviderRows *rows, CatalogView *view, const char *name)
{
	Oid			result;
	int32		typmod;
	Size		len = strlen(name);

	if (!len)
		return InvalidOid;
	if (len >= 2 && !strcmp(name + len - 2, "[]"))
	{
		Oid			element = routine_type_oid(rows, view, pnstrdup(name, len - 2));
		Oid			array = provider_array_of(rows, element);
		ListCell   *lc;

		if (OidIsValid(array))
			return array;
		/* A built-in or extension element type has a native array type. */
		if (element < FirstDurableObjectId)
			return get_array_type(element);
		foreach(lc, rows->rows[provider_catalog_index(TypeRelationId)])
		{
			Form_pg_type type = (Form_pg_type) GETSTRUCT((HeapTuple) lfirst(lc));

			if (type->oid == element && type->typarray)
				return type->typarray;
		}
		elog(ERROR, "canonical array type is missing: %s", name);
	}
	result = CatalogViewFindObject(view, TypeRelationId, name);
	if (OidIsValid(result))
		return result;
	if (strncmp(name, "pg_catalog.", strlen("pg_catalog.")) && OidIsValid(result = extension_type_oid(name)))
		return result;
	parseTypeString(name, &result, &typmod, false);
	if (strncmp(name, "pg_catalog.", strlen("pg_catalog.")))
		unsupported("routine signature resolving outside canonical catalog");
	return result;
}

static void
provider_routine(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	int			slot = provider_catalog_index(ProcedureRelationId);
	TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
	Datum		values[Natts_pg_proc] = {0};
	bool		nulls[Natts_pg_proc] = {0};
	HeapTuple	tuple;
	Form_pg_proc proc;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		char	   *encoded;
		Oid			input,
					param;

		if (i + 1 == Anum_pg_proc_oid)
		{
			values[i] = ObjectIdGetDatum(CatalogViewBindObject(view, ProcedureRelationId, json_value(definition, "name"), InvalidOid));
			continue;
		}
		encoded = json_value(definition, NameStr(attr->attname));
		if (!encoded)
		{
			nulls[i] = true;
			continue;
		}
		switch (i + 1)
		{
			case Anum_pg_proc_pronamespace:
				values[i] = ObjectIdGetDatum(CatalogViewLookupObject(view, NamespaceRelationId, encoded));
				break;
			case Anum_pg_proc_proowner:
				values[i] = ObjectIdGetDatum(get_role_oid(encoded, false));
				break;
			case Anum_pg_proc_prolang:
				values[i] = ObjectIdGetDatum(get_language_oid(encoded, false));
				break;
			case Anum_pg_proc_prorettype:
			case Anum_pg_proc_provariadic:
				values[i] = ObjectIdGetDatum(routine_type_oid(rows, view, encoded));
				break;
			case Anum_pg_proc_proargtypes:
			case Anum_pg_proc_proallargtypes:
				{
					ArrayType  *array = DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN, encoded, TEXTOID, -1));
					Datum	   *names,
							   *items;
					bool	   *missing;
					int			n;
					Oid		   *ids;

					deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT, &names, &missing, &n);
					ids = palloc_array(Oid, Max(n, 1));
					items = palloc_array(Datum, Max(n, 1));
					for (int j = 0; j < n; j++)
					{
						if (missing[j])
							elog(ERROR, "null canonical argument type");
						ids[j] = routine_type_oid(rows, view, TextDatumGetCString(names[j]));
						items[j] = ObjectIdGetDatum(ids[j]);
					}
					values[i] = i + 1 == Anum_pg_proc_proargtypes ? PointerGetDatum(buildoidvector(ids, n)) : PointerGetDatum(construct_array(items, n, OIDOID, sizeof(Oid), true, TYPALIGN_INT));
					break;
				}
			case Anum_pg_proc_proargdefaults:
				/* Bound below, once the argument types are known. */
				break;
			case Anum_pg_proc_prosqlbody:
				/* Analyzed when first read: see dg_routine_body. */
				values[i] = CStringGetTextDatum(nodeToString(makeString(encoded)));
				break;
			default:
				getTypeInputInfo(attr->atttypid, &input, &param);
				values[i] = OidInputFunctionCall(input, encoded, param, -1);
				break;
		}
	}
	if (!nulls[Anum_pg_proc_proargdefaults - 1])
	{
		/* Defaults belong to the last input arguments, as in CreateFunction. */
		oidvector  *args = (oidvector *) DatumGetPointer(values[Anum_pg_proc_proargtypes - 1]);
		ArrayType  *array = DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN, json_value(definition, "proargdefaults"), TEXTOID, -1));
		Datum	   *sql;
		bool	   *missing;
		int			n;
		List	   *defaults = NIL;

		deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT, &sql, &missing, &n);
		for (int j = 0; j < n; j++)
			defaults = lappend(defaults,
							   schema_expression_bind_default(schema_expression_raw(TextDatumGetCString(sql[j])),
															  args->values[args->dim1 - n + j]));
		values[Anum_pg_proc_proargdefaults - 1] = CStringGetTextDatum(nodeToString(defaults));
	}
	tuple = heap_form_tuple(desc, values, nulls);
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	provider_dependency(rows, ProcedureRelationId, proc->oid, 0, NamespaceRelationId, proc->pronamespace, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, ProcedureRelationId, proc->oid, 0, LanguageRelationId, proc->prolang, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, ProcedureRelationId, proc->oid, 0, TypeRelationId, proc->prorettype, 0, DEPENDENCY_NORMAL);
	if (!nulls[Anum_pg_proc_proallargtypes - 1])
	{
		ArrayType  *all = DatumGetArrayTypeP(values[Anum_pg_proc_proallargtypes - 1]);
		Oid		   *ids = (Oid *) ARR_DATA_PTR(all);
		int			n = ArrayGetNItems(ARR_NDIM(all), ARR_DIMS(all));

		for (int i = 0; i < n; i++)
			provider_dependency(rows, ProcedureRelationId, proc->oid, 0, TypeRelationId, ids[i], 0, DEPENDENCY_NORMAL);
	}
	else
		for (int i = 0; i < proc->pronargs; i++)
			provider_dependency(rows, ProcedureRelationId, proc->oid, 0, TypeRelationId, proc->proargtypes.values[i], 0, DEPENDENCY_NORMAL);

	/* What a SQL-standard body uses, as CreateFunction records it. */
	{
		int			n;
		JsonbValue *dependencies = json_elements(definition, "dependencies", &n);
		char	  **kinds = palloc_array(char *, Max(n, 1));
		char	  **names = palloc_array(char *, Max(n, 1));

		for (int i = 0; i < n; i++)
		{
			kinds[i] = json_item_text(&dependencies[i], 0);
			names[i] = json_item_text(&dependencies[i], 1);
		}
		for (int i = 0; i < n; i++)
		{
			Oid			referenced = provider_view_dependency(view, kinds[i], names[i]);

			if (OidIsValid(referenced))
				provider_dependency(rows, ProcedureRelationId, proc->oid, 0,
									!strcmp(kinds[i], "table") ? RelationRelationId : !strcmp(kinds[i], "routine") ? ProcedureRelationId : !strcmp(kinds[i], "operator") ? OperatorRelationId : TypeRelationId,
									referenced, 0, DEPENDENCY_NORMAL);
		}
	}
}
