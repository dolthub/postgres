/* All identities are bound before resolving cross-object references, including
 * operator shells and mutual commutators. */
static void
provider_schema_object(DGProviderRows *rows, CatalogView *view, const char *kind, const char *definition)
{
	Oid objectclass = schema_object_class(kind);
	Oid catalog = objectclass == ProcedureRelationId ? AggregateRelationId : objectclass;
	Oid id = CatalogViewLookupObject(view, objectclass, json_value(definition, "name"));
	int slot = provider_catalog_index(catalog);
	TupleDesc desc = RelationGetDescr(rows->relations[slot]);
	Datum *values = palloc0_array(Datum, desc->natts);
	bool *nulls = palloc0_array(bool, desc->natts);
	char *row = json_value(definition, "row");

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		const char *field = NameStr(attr->attname);
		char *encoded = json_value(row, field);
		Oid reference = schema_field_class(catalog, field);

		if (!strcmp(field, "oid"))
		{
			values[i] = ObjectIdGetDatum(id);
			continue;
		}
		if (!encoded)
		{
			nulls[i] = true;
			continue;
		}
		if (reference)
		{
			Oid target = InvalidOid;

			if (encoded[0])
			{
				if (reference == TypeRelationId)
					target = routine_type_oid(rows, view, encoded);
				else if (reference == AuthIdRelationId)
					target = provider_role(encoded);
				else
				{
					target = CatalogViewFindObject(view, reference, encoded);
					if (!target && reference == ProcedureRelationId)
						target = DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum(encoded)));
					if (!target && reference == OperatorRelationId)
						target = DatumGetObjectId(DirectFunctionCall1(regoperatorin, CStringGetDatum(encoded)));
					if (!target)
						elog(ERROR, "missing canonical schema reference %s", encoded);
				}
			}
			values[i] = ObjectIdGetDatum(target);

			/* As natively, an operator does not depend on its commutator or negator. */
			if (target && target != id && reference != AuthIdRelationId &&
				strcmp(field, "oprcom") && strcmp(field, "oprnegate"))
				provider_dependency(rows, objectclass, id, 0, reference, target, 0, DEPENDENCY_NORMAL);
		}
		else
		{
			Oid input, param;

			getTypeInputInfo(attr->atttypid, &input, &param);
			values[i] = OidInputFunctionCall(input, encoded, param, -1);
		}
	}
	rows->rows[slot] = lappend(rows->rows[slot], heap_form_tuple(desc, values, nulls));
	provider_description(rows, id, objectclass, 0, json_value(definition, "comment"));
}
