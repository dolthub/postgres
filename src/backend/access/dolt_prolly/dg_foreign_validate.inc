/* Use the FK's actual equality operators, including native input coercions. */
static const char *
foreign_violation_action(char action)
{
 switch (action)
 {
  case FKCONSTR_ACTION_CASCADE: return "CASCADE";
  case FKCONSTR_ACTION_SETNULL: return "SET NULL";
  case FKCONSTR_ACTION_SETDEFAULT: return "SET DEFAULT";
  default: return "RESTRICT";
 }
}

static void
collect_foreign_violations(HeapTuple constraint, Relation fk, Relation pk, StringInfo violations)
{
 Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraint);
 Datum *fkcols, *pkcols, *operators;
 bool *nulls;
 bool isnull;
 int n, np, no;
 StringInfoData query, present, missing, join, columns, referenced, info;
 const char *index_name = NameStr(con->conname);
 List *indexes;
 ListCell *lc;
 TupleTableSlot *slot;
 SPITupleTable *rows;
 uint64 count;

 deconstruct_array(DatumGetArrayTypeP(SysCacheGetAttr(CONSTROID, constraint, Anum_pg_constraint_conkey, &isnull)),
  INT2OID, 2, true, TYPALIGN_SHORT, &fkcols, &nulls, &n);
 deconstruct_array(DatumGetArrayTypeP(SysCacheGetAttr(CONSTROID, constraint, Anum_pg_constraint_confkey, &isnull)),
  INT2OID, 2, true, TYPALIGN_SHORT, &pkcols, &nulls, &np);
 deconstruct_array(DatumGetArrayTypeP(SysCacheGetAttr(CONSTROID, constraint, Anum_pg_constraint_conpfeqop, &isnull)),
  OIDOID, sizeof(Oid), true, TYPALIGN_INT, &operators, &nulls, &no);
 if (n != np || n != no)
  elog(ERROR, "invalid foreign key column metadata");
 initStringInfo(&query); initStringInfo(&present); initStringInfo(&missing);
 initStringInfo(&join); initStringInfo(&columns); initStringInfo(&referenced); initStringInfo(&info);
 appendStringInfoChar(&columns, '['); appendStringInfoChar(&referenced, '[');
 for (int i=0; i<n; i++)
 {
  const char *f = NameStr(TupleDescAttr(RelationGetDescr(fk), DatumGetInt16(fkcols[i])-1)->attname);
  const char *p = NameStr(TupleDescAttr(RelationGetDescr(pk), DatumGetInt16(pkcols[i])-1)->attname);
  HeapTuple optuple = SearchSysCache1(OPEROID, operators[i]);
  Form_pg_operator op = (Form_pg_operator) GETSTRUCT(optuple);
  if (i) { appendStringInfoString(&present, con->confmatchtype == FKCONSTR_MATCH_FULL ? " OR " : " AND "); appendStringInfoString(&missing," OR "); appendStringInfoString(&join," AND "); appendStringInfoChar(&columns,','); appendStringInfoChar(&referenced,','); }
  appendStringInfo(&present,"f.%s IS NOT NULL", dg_quote_identifier(f));
  appendStringInfo(&missing,"f.%s IS NULL", dg_quote_identifier(f));
  appendStringInfo(&join,"(p.%s)::%s OPERATOR(%s.%s) (f.%s)::%s",dg_quote_identifier(p),format_type_be_qualified(op->oprleft),dg_quote_identifier(get_namespace_name(op->oprnamespace)),NameStr(op->oprname),dg_quote_identifier(f),format_type_be_qualified(op->oprright));
  escape_json(&columns,f); escape_json(&referenced,p);
  ReleaseSysCache(optuple);
 }
 appendStringInfoChar(&columns, ']'); appendStringInfoChar(&referenced, ']');
 appendStringInfo(&query,"SELECT f FROM %s f WHERE (%s) AND (",dg_qualified_name(get_namespace_name(RelationGetNamespace(fk)),RelationGetRelationName(fk)),present.data);
 if (con->confmatchtype == FKCONSTR_MATCH_FULL)
  appendStringInfo(&query,"(%s) OR ",missing.data);
 appendStringInfo(&query,"NOT EXISTS (SELECT 1 FROM %s p WHERE %s))",dg_qualified_name(get_namespace_name(RelationGetNamespace(pk)),RelationGetRelationName(pk)),join.data);
 indexes = RelationGetIndexList(fk);
 foreach(lc,indexes)
 {
  HeapTuple index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
  Form_pg_index index = (Form_pg_index) GETSTRUCT(index_tuple);
  bool matches = index->indnkeyatts >= n;
  for(int i=0; matches && i<n; i++) matches = index->indkey.values[i] == DatumGetInt16(fkcols[i]);
  if(matches) index_name = pstrdup(get_rel_name(lfirst_oid(lc)));
  ReleaseSysCache(index_tuple);
  if(matches) break;
 }
 appendStringInfo(&info,"{\"Columns\":%s,\"ForeignKey\":",columns.data);
 escape_json(&info,NameStr(con->conname));
 appendStringInfoString(&info,",\"Index\":"); escape_json(&info,index_name);
 appendStringInfoString(&info,",\"OnDelete\":"); escape_json(&info,foreign_violation_action(con->confdeltype));
 appendStringInfoString(&info,",\"OnUpdate\":"); escape_json(&info,foreign_violation_action(con->confupdtype));
 appendStringInfo(&info,",\"ReferencedColumns\":%s,\"ReferencedIndex\":",referenced.data);
 escape_json(&info,get_rel_name(con->conindid));
 appendStringInfoString(&info,",\"ReferencedTable\":"); escape_json(&info,RelationGetRelationName(pk));
 appendStringInfoString(&info,",\"Table\":"); escape_json(&info,RelationGetRelationName(fk)); appendStringInfoChar(&info,'}');
 run(query.data,0,NULL,NULL,NULL,true);
 rows = SPI_tuptable; count = SPI_processed;
 if (count && !dolt_force_transaction_commit)
  ereport(ERROR,(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),errmsg("constraint violations: foreign key \"%s\" on table \"%s\"",NameStr(con->conname),RelationGetRelationName(fk)),errhint("Set dolt_force_transaction_commit=1 to retain merge violations for repair.")));
 slot = MakeSingleTupleTableSlot(RelationGetDescr(fk), &TTSOpsHeapTuple);
 for(uint64 i=0; i<count; i++)
 {
  HeapTupleHeader header = DatumGetHeapTupleHeader(SPI_getbinval(rows->vals[i],rows->tupdesc,1,&isnull));
  HeapTupleData tuple;
  PLSlice packed;
  tuple.t_len = HeapTupleHeaderGetDatumLength(header); tuple.t_data=header;
  ItemPointerSetInvalid(&tuple.t_self); tuple.t_tableOid=RelationGetRelid(fk);
  ExecStoreHeapTuple(&tuple,slot,false);
  packed=dg_encode(fk,slot);
  if(violations->len) appendStringInfoChar(violations,',');
  appendStringInfoString(violations,"{\"table\":");
  escape_json(violations,dg_qualified_name(get_namespace_name(RelationGetNamespace(fk)),RelationGetRelationName(fk)));
  appendStringInfoString(violations,",\"type\":\"foreign key\",\"row\":"); json_bytes(violations,DG_VALUE(true,packed));
  appendStringInfo(violations,",\"info\":%s,\"info_text\":",info.data); escape_json(violations,info.data); appendStringInfoChar(violations,'}');
  ExecClearTuple(slot);
 }
 ExecDropSingleTupleTableSlot(slot);
}

/* Root recombination bypasses DML triggers. Check the complete resulting graph
 * after all tables are installed, while their branch writer locks remain held.
 * Whole-database restore/fast-forward uses an already retained root instead.
 * Until Dolt FK violation records are supported, violations abort publication;
 * this also conservatively checks pre-existing NOT VALID constraints. */
static void
validate_working_foreign_keys(const char *branch, StringInfo violations)
{
	bytea	   *database = read_ref(psprintf("working/%s", branch)).target;
	CatalogView *view;
	CatalogView *previous;
	const char *previous_branch;
	List	   *constraints;
	ListCell   *lc;
	int			depth = catalog_api_depth;
	Oid			user;
	int			security;

	if (!database)
		return;
	view = provider_view(database);
	CatalogViewEnsureLoaded(view);
	constraints = CatalogViewTuples(view, ConstraintRelationId);
	previous = CatalogViewSwitch(view);
	previous_branch = dg_switch_execution_branch(branch);
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		catalog_api_depth = 0;
		foreach(lc, constraints)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT((HeapTuple) lfirst(lc));
			Relation	fk;
			Relation	pk;
			Trigger		trigger = {0};

			if (con->contype != CONSTRAINT_FOREIGN)
				continue;
			fk = dg_catalog_trigger_relation(con->conrelid, branch);
			pk = dg_catalog_trigger_relation(con->confrelid, branch);
			dg_catalog_foreign_lock(fk);
			dg_catalog_foreign_lock(pk);
			trigger.tgconstraint = con->oid;
			trigger.tgconstrrelid = con->confrelid;
			trigger.tgconstrindid = con->conindid;
			trigger.tgname = NameStr(con->conname);
			if (violations)
				collect_foreign_violations((HeapTuple) lfirst(lc), fk, pk, violations);
			else if (!RI_Initial_Check(&trigger, fk, pk))
				elog(ERROR, "cannot validate foreign key root publication");
			dg_storage_close_relation(pk);
			dg_storage_close_relation(fk);
		}
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		catalog_api_depth = depth;
		dg_switch_execution_branch(previous_branch);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}
