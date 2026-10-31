static void
provider_sequence(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	FormData_pg_class form = {0};
	FormData_pg_sequence seq = {0};
	TupleDesc	desc = CreateTemplateTupleDesc(3);
	Oid			resource = sequence_resource(definition, false);

	form.oid = CatalogViewFindObject(view, RelationRelationId, name);
	namestrcpy(&form.relname, strVal(lsecond(parts)));
	form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	form.relowner = provider_role(json_value(definition, "owner"));
	form.relkind = RELKIND_SEQUENCE;
	form.relpersistence = RELPERSISTENCE_PERMANENT;
	form.relnatts = 3;
	form.relispopulated = true;
	form.relreplident = REPLICA_IDENTITY_NOTHING;
	if (OidIsValid(resource))
	{
		RelFileLocator locator;

		dg_storage_locator(resource, &locator);
		form.relfilenode = locator.relNumber;
	}
	{
		int			slot = provider_catalog_index(RelationRelationId);
		HeapTuple	tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_class_relminmxid);

		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_class_relacl, PointerGetDatum(provider_acl(definition)));
		rows->rows[slot] = lappend(rows->rows[slot], tuple);
	}
	provider_dependency(rows, RelationRelationId, form.oid, 0, NamespaceRelationId, form.relnamespace, 0, DEPENDENCY_NORMAL);
	{
		char	   *owned = json_value(definition, "owned_table");

		if (owned)
		{
			Oid			table = CatalogViewLookupObject(view, RelationRelationId, owned);
			int			member = provider_member(owned, NULL);
			int			column = provider_attribute(provider_columns(provider_definition_list[member]), json_value(definition, "owned_column"));

			provider_dependency(rows, RelationRelationId, form.oid, 0, RelationRelationId, table, column, !strcmp(json_value(definition, "identity"), "true") ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);
		}
	}
	TupleDescInitEntry(desc, 1, "last_value", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 2, "log_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 3, "is_called", BOOLOID, -1, 0);
	for (int i = 0; i < 3; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		attr->attrelid = form.oid;
		attr->attnotnull = true;
		provider_add(rows, AttributeRelationId, attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
	}
	FreeTupleDesc(desc);
	seq.seqrelid = form.oid;
	seq.seqtypid = !strcmp(json_value(definition, "type"), "int2") ? INT2OID : !strcmp(json_value(definition, "type"), "int4") ? INT4OID : INT8OID;
	seq.seqstart = sequence_number(definition, "start");
	seq.seqincrement = sequence_number(definition, "increment");
	seq.seqmin = sequence_number(definition, "min");
	seq.seqmax = sequence_number(definition, "max");
	seq.seqcache = sequence_number(definition, "cache");
	seq.seqcycle = !strcmp(json_value(definition, "cycle"), "true");
	provider_add(rows, SequenceRelationId, &seq, sizeof(seq), Anum_pg_sequence_seqcycle);
}
