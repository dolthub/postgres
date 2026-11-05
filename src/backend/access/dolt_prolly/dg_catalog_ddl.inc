/* First writable provider operation: remove branch membership, not storage.
 * The physical catalog remains a runtime bridge for surviving definitions. */
void
dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot)
{
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	if (!IsUnderPostmaster || CatalogViewIsProvisional(rel->rd_catalogview))
		return;
	GetUserIdAndSecContext(&user, &security);
	if (!ActiveSnapshotSet() || (snapshot && IsMVCCSnapshot(snapshot)))
	{
		PushActiveSnapshot(snapshot && IsMVCCSnapshot(snapshot) ? snapshot : GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;
		char	   *name = dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));

		nest = begin_api();
		db = catalog_database(branch);
		if (!catalog_contains(db, name))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", name, branch)));
		if (db)
		{
			Oid			bound;
			bytea	   *schema;

			{
				bytea	   *member = dg_database_member(db, "table", name);

				if (!member)
					member = dg_database_member(db, "index", name);
				if (!member)
					elog(ERROR, "relation %s has no member on branch %s", name, branch);
				schema = child(member, 1);
			}
			bound = binding_relation(branch, name, schema);
			if (bound != RelationGetRelid(rel))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding mismatch for %s on branch %s", name, branch)));
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
}

bool
dg_catalog_drop(Node *statement)
{
	return canonical_drop(statement);
}
