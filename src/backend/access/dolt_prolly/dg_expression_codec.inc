/*
 * Whether native object |objid| of |classid| belongs to an extension. Such an
 * object is named, like a built-in one, by its qualified name: a server with
 * the extension installed binds it again.
 */
static bool
schema_extension_member(Oid classid, Oid objid)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	Oid			extension = InvalidOid;

	PG_TRY();
	{
		extension = getExtensionOfObject(classid, objid);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return OidIsValid(extension);
}

/* Whether |objid| is a native object local to this server, outside any extension. */
static bool
schema_expression_local(Oid classid, Oid objid)
{
	return objid >= FirstNormalObjectId && objid < FirstDurableObjectId &&
		!schema_extension_member(classid, objid);
}

/* Expression payloads contain SQL and symbolic column names, never serialized
 * executor nodes or local OIDs. Rebinding uses native expression transformation.
 * The first codec deliberately admits only built-in scalar expression families;
 * adding a node requires auditing every identity and datum it contains. */
static bool
schema_expression_check(Node *node, void *context)
{
	if (!node)
		return false;
	switch (nodeTag(node))
	{
		case T_List:
		case T_CaseWhen:
			return expression_tree_walker(node, schema_expression_check, context);
		case T_Var:
			if (((Var *) node)->varno != 1 || ((Var *) node)->varattno <= 0 || ((Var *) node)->varlevelsup)
				unsupported("canonical expression whole-row or external reference");
			break;
		case T_Const:

			/*
			 * A relation reference, such as a sequence for nextval(), is
			 * deparsed by name and rebound by name: portable SQL, never an OID.
			 */
			if (((Const *) node)->consttype == REGCLASSOID)
				return false;
			break;
		case T_FuncExpr:

			/*
			 * A versioned routine or an extension's function is deparsed by
			 * qualified name and rebound by name; any other native user
			 * function is local to this server.
			 */
			if (schema_expression_local(ProcedureRelationId, ((FuncExpr *) node)->funcid))
				unsupported("canonical expression user-defined function");
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			if (schema_expression_local(OperatorRelationId, ((OpExpr *) node)->opno))
				unsupported("canonical expression user-defined operator");
			break;
		case T_ScalarArrayOpExpr:
			if (schema_expression_local(OperatorRelationId, ((ScalarArrayOpExpr *) node)->opno))
				unsupported("canonical expression user-defined operator");
			break;
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_ArrayExpr:
		case T_ArrayCoerceExpr:
		case T_RelabelType:
		case T_CoerceViaIO:
		case T_CollateExpr:
		case T_SQLValueFunction:
			break;
		default:
			unsupported("canonical scalar expression node codec");
	}
	if (schema_expression_local(TypeRelationId, exprType(node)))
		unsupported("canonical expression user-defined result type");
	if (!context || exprType(node) != VOIDOID)
	{
		TupleDesc	desc = CreateTemplateTupleDesc(1);

		TupleDescInitEntry(desc, 1, "value", exprType(node), -1, 0);
		dg_validate_descriptor(desc);
		FreeTupleDesc(desc);
	}
	return expression_tree_walker(node, schema_expression_check, context);
}

static void
schema_expression_gucs(void)
{
	set_config_option("TimeZone", "UTC", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("DateStyle", "ISO, YMD", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("IntervalStyle", "postgres", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("extra_float_digits", "3", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("bytea_output", "hex", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("standard_conforming_strings", "on", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

	/* User relations, such as sequences, are always written schema-qualified. */
	set_config_option("search_path", "", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
}

static List *
schema_expression_parse(const char *sql)
{
	List	   *result = NIL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		result = raw_parser(sql, RAW_PARSE_DEFAULT);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

/* The raw parse tree of one stored expression. */
static Node *
schema_expression_raw(const char *sql)
{
	List	   *parsed = schema_expression_parse(psprintf("SELECT %s", sql));
	SelectStmt *select;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, SelectStmt))
		elog(ERROR, "invalid canonical expression");
	select = castNode(SelectStmt, linitial_node(RawStmt, parsed)->stmt);
	if (list_length(select->targetList) != 1 || select->fromClause)
		elog(ERROR, "invalid canonical expression SELECT");
	return linitial_node(ResTarget, select->targetList)->val;
}

static char *
schema_expression_sql(Node *expression, Oid relation)
{
	int			nest = NewGUCNestLevel();
	char	   *result = NULL;

	(void) schema_expression_check(expression, NULL);
	PG_TRY();
	{
		schema_expression_gucs();
		result = deparse_expression(expression,
									OidIsValid(relation) ? deparse_context_for(get_rel_name(relation), relation) : NIL,
									false, false);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

/* attribute: positive = default/generated column, zero = CHECK, -1 =
 * ALTER USING, -2 = index expression, -3 = index predicate. Index trees are
 * bound from canonical SQL and contain runtime identities only. */
static Node *
schema_expression_bind(CatalogView *view, Oid relation, const char *name,
					   TupleDesc desc, Node *raw, int attribute)
{
	ParseState *pstate = make_parsestate(NULL);
	RelationData definition = {0};
	FormData_pg_class form = {0};
	ParseNamespaceItem *nsitem;
	Node	   *result = NULL;
	int			nest = NewGUCNestLevel();
	CatalogView *previous = CatalogViewCurrent();

	/*
	 * Bind column Vars from the supplied canonical descriptor. No physical
	 * relation or partially built relcache entry participates in this parse.
	 */
	definition.rd_id = relation;
	definition.rd_att = desc;
	definition.rd_rel = &form;
	definition.rd_catalogview = view;
	form.relkind = RELKIND_RELATION;
	namestrcpy(&form.relname, name);
	nsitem = addRangeTableEntryForRelation(pstate, &definition, AccessShareLock, NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);
	PG_TRY();
	{
		/*
		 * Routines, types and regclass constants resolve in their
		 * definition's view.
		 */
		CatalogViewSwitch(view);
		if (attribute != -1)
			schema_expression_gucs();
		if (attribute > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, attribute - 1);

			if (attr->attgenerated)
			{
				/*
				 * As cookDefault, but the native DDL that stored this
				 * expression has already checked the generated-column rules,
				 * which the half-built view cannot answer.
				 */
				result = transformExpr(pstate, raw, EXPR_KIND_GENERATED_COLUMN);
				result = coerce_to_target_type(pstate, result, exprType(result), attr->atttypid, attr->atttypmod,
											   COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
				if (!result)
					elog(ERROR, "canonical generation expression has the wrong type");
				assign_expr_collations(pstate, result);
			}
			else
				result = cookDefault(pstate, raw, attr->atttypid, attr->atttypmod, NameStr(attr->attname), 0);
		}
		else if (attribute == -2 || attribute == -3)
		{
			result = transformExpr(pstate, copyObject(raw), attribute == -2 ? EXPR_KIND_INDEX_EXPRESSION : EXPR_KIND_INDEX_PREDICATE);
			if (attribute == -3)
				result = coerce_to_boolean(pstate, result, "WHERE");
			assign_expr_collations(pstate, result);
		}
		else if (attribute < 0)
		{
			result = transformExpr(pstate, copyObject(raw), EXPR_KIND_ALTER_COL_TRANSFORM);
			assign_expr_collations(pstate, result);
		}
		else
		{
			result = transformExpr(pstate, raw, EXPR_KIND_CHECK_CONSTRAINT);
			result = coerce_to_boolean(pstate, result, "CHECK");
			assign_expr_collations(pstate, result);
		}
		(void) schema_expression_check(result, attribute == -1 ? (void *) pstate : NULL);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		AtEOXact_GUC(true, nest);
		free_parsestate(pstate);
	}
	PG_END_TRY();
	return result;
}

/* Bind a routine parameter default, as CreateFunction does. */
static Node *
schema_expression_bind_default(Node *raw, Oid type)
{
	ParseState *pstate = make_parsestate(NULL);
	Node	   *result = NULL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		result = transformExpr(pstate, raw, EXPR_KIND_FUNCTION_DEFAULT);
		result = coerce_to_specific_type(pstate, result, type, "DEFAULT");
		assign_expr_collations(pstate, result);
		(void) schema_expression_check(result, NULL);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		free_parsestate(pstate);
	}
	PG_END_TRY();
	return result;
}

/*
 * Bind a trigger's WHEN clause, as CreateTrigger does: OLD and NEW are the
 * first and second range table entries. Routines it calls resolve in |view|.
 */
static Node *
schema_expression_bind_trigger(CatalogView *view, Oid relation, const char *name,
							   TupleDesc desc, Node *raw)
{
	ParseState *pstate = make_parsestate(NULL);
	RelationData definition = {0};
	FormData_pg_class form = {0};
	Node	   *result = NULL;
	int			nest = NewGUCNestLevel();
	CatalogView *previous = CatalogViewCurrent();

	definition.rd_id = relation;
	definition.rd_att = desc;
	definition.rd_rel = &form;
	definition.rd_catalogview = view;
	form.relkind = RELKIND_RELATION;
	namestrcpy(&form.relname, name);
	addNSItemToQuery(pstate, addRangeTableEntryForRelation(pstate, &definition, AccessShareLock,
														   makeAlias("old", NIL), false, false),
					 false, true, true);
	addNSItemToQuery(pstate, addRangeTableEntryForRelation(pstate, &definition, AccessShareLock,
														   makeAlias("new", NIL), false, false),
					 false, true, true);
	PG_TRY();
	{
		schema_expression_gucs();
		/* OLD.* and NEW.* need the canonical relation row even without calls. */
		CatalogViewSwitch(view);
		result = transformWhereClause(pstate, raw, EXPR_KIND_TRIGGER_WHEN, "WHEN");
		assign_expr_collations(pstate, result);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		AtEOXact_GUC(true, nest);
		free_parsestate(pstate);
	}
	PG_END_TRY();
	return result;
}

/* Whether a stored expression calls a native, unversioned user function or operator. */
static bool
expression_calls_native(Node *node, void *context)
{
	if (!node)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstNormalObjectId &&
		((FuncExpr *) node)->funcid < FirstDurableObjectId)
		return true;
	if (IsA(node, OpExpr) && ((OpExpr *) node)->opno >= FirstNormalObjectId && ((OpExpr *) node)->opno < FirstDurableObjectId)
		return true;
	return expression_tree_walker(node, expression_calls_native, context);
}

typedef struct DomainValue
{
	Oid			type;
	int32		typmod;
	Oid			collation;
} DomainValue;

/* As DefineDomain: VALUE in a domain constraint is the value being checked. */
static Node *
schema_expression_domain_value(ParseState *pstate, ColumnRef *ref)
{
	DomainValue *value = pstate->p_ref_hook_state;
	CoerceToDomainValue *result;

	if (list_length(ref->fields) != 1 || !IsA(linitial(ref->fields), String) ||
		strcmp(strVal(linitial(ref->fields)), "value"))
		return NULL;
	result = makeNode(CoerceToDomainValue);
	result->typeId = value->type;
	result->typeMod = value->typmod;
	result->collation = value->collation;
	result->location = ref->location;
	return (Node *) result;
}

/*
 * Bind a domain's default, or with |check| one of its CHECK constraints,
 * against its base type, as DefineDomain does.
 */
static Node *
schema_expression_bind_domain(CatalogView *view, Node *raw, const char *name, Oid base,
							  int32 typmod, Oid collation, bool check)
{
	ParseState *pstate = make_parsestate(NULL);
	DomainValue value = {
		.type = base,
		.typmod = typmod,
		.collation = collation,
	};
	Node	   *result = NULL;
	int			nest = NewGUCNestLevel();
	CatalogView *previous = CatalogViewCurrent();

	PG_TRY();
	{
		schema_expression_gucs();
		CatalogViewSwitch(view);
		if (check)
		{
			pstate->p_pre_columnref_hook = schema_expression_domain_value;
			pstate->p_ref_hook_state = &value;
			result = transformExpr(pstate, raw, EXPR_KIND_DOMAIN_CHECK);
			result = coerce_to_boolean(pstate, result, "CHECK");
			assign_expr_collations(pstate, result);
		}
		else
			result = cookDefault(pstate, raw, base, typmod, name, 0);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		AtEOXact_GUC(true, nest);
		free_parsestate(pstate);
	}
	PG_END_TRY();
	return result;
}
