/* Dolt system tables: the dolt namespace and dolt_-prefixed aliases.
 *
 * Doltgres exposes version-control system tables both in an implicit "dolt"
 * schema (dolt.branches) and with a dolt_ prefix in every schema
 * (dolt_branches, public.dolt_branches). The dolt schema and its views are
 * created by initdb; the pg_catalog aliases are found by the ordinary search
 * path. A schema-qualified alias resolves here when the schema exists but
 * holds no relation of that name. */
#include "dg.h"
#include "access/doltsystem.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace_d.h"
#include "utils/lsyscache.h"
#include "utils/acl.h"

bool		dolt_show_system_tables = false;

/* The dolt schema holds built-in definitions, like pg_catalog. */
bool
dg_system_namespace(const char *name)
{
	return name && (!strcmp(name, "pg_catalog") || !strcmp(name, "information_schema") || !strcmp(name, "dolt"));
}

/* Resolve schema.dolt_<name> to the pg_catalog alias when the schema exists.
 * A relation the user created with that name in the schema takes precedence;
 * callers only reach this after their own lookup failed. */
Oid
dg_system_relation(const RangeVar *rv)
{
	Oid			id;

	if (!rv->schemaname || strncmp(rv->relname, "dolt_", 5) != 0)
		return InvalidOid;
	if (rv->relpersistence == RELPERSISTENCE_TEMP)
		return InvalidOid;
	if (!OidIsValid(get_namespace_oid(rv->schemaname, true)))
		return InvalidOid;
	id = get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE);
	if (!OidIsValid(id) || get_rel_relkind(id) != RELKIND_VIEW)
		return InvalidOid;
	return id;
}

/* ---- Generated per-table system tables and dynamic table functions ----
 *
 * dolt_diff_<table>, dolt_commit_diff_<table>, dolt_history_<table>,
 * dolt_blame_<table> and dolt_workspace_<table> are rewritten in the parser
 * into set-returning function calls with an explicit column definition list
 * computed from the table's schema. The same column service lets
 * dolt_diff(from, to, table) be used without a column definition list. */

#include "catalog/namespace.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "executor/executor.h"
#include "parser/analyze.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"

typedef struct GeneratedTable
{
	const char *prefix;
	const char *kind;
	const char *function;
	bool		hints;			/* accepts from/to commit hints */
	bool		optional;		/* unknown base table yields an empty result */
}			GeneratedTable;

static const GeneratedTable generated_tables[] = {
	{"dolt_commit_diff_", "commit_diff", "dolt_commit_diff_table", true, false},
	{"dolt_diff_", "diff", "dolt_diff_table", true, false},
	{"dolt_history_", "history", "dolt_history_table", false, false},
	{"dolt_blame_", "blame", "dolt_blame_table", false, false},
	{"dolt_workspace_", "workspace", "dolt_workspace_table", false, true},
	{"dolt_conflicts_", "conflicts", "dolt_conflicts_table", false, false},
	{"dolt_constraint_violations_", "violations", "dolt_constraint_violations_table", false, false},
};

/* Generated tables whose rows can be deleted: the delete clears the pending
 * merge entries of the table. */
static const struct
{
	const char *prefix;
	const char *function;
}			deletable_tables[] =

{
	{"dolt_conflicts_", "dolt_conflicts_clear"},
	{"dolt_constraint_violations_", "dolt_constraint_violations_clear"},
};

static Node *
text_constant(const char *value)
{
	A_Const    *c = makeNode(A_Const);

	if (!value)
	{
		TypeCast   *cast = makeNode(TypeCast);

		c->isnull = true;
		c->location = -1;
		cast->arg = (Node *) c;
		cast->typeName = makeTypeNameFromNameList(list_make2(makeString("pg_catalog"), makeString("text")));
		cast->location = -1;
		return (Node *) cast;
	}
	c->val.sval.type = T_String;
	c->val.sval.sval = pstrdup(value);
	c->location = -1;
	return (Node *) c;
}

/* Evaluate a raw argument expression to a text constant at parse time.
 * Non-constant expressions (parameters, column references) return NULL. */
static char *
constant_text(ParseState *pstate, Node *raw)
{
	Node	   *expr;
	Const	   *c;

	if (IsA(raw, ColumnRef))
		return NULL;
	expr = transformExpr(pstate, copyObject(raw), EXPR_KIND_OTHER);
	expr = coerce_to_target_type(pstate, expr, exprType(expr), TEXTOID, -1, COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	if (!expr || contain_var_clause(expr) || contain_volatile_functions(expr))
		return NULL;
	expr = (Node *) evaluate_expr((Expr *) expr, TEXTOID, -1, InvalidOid);
	if (!IsA(expr, Const))
		return NULL;
	c = (Const *) expr;
	if (c->constisnull)
		return NULL;
	return TextDatumGetCString(c->constvalue);
}

static List *
layout_columns(TupleDesc desc)
{
	List	   *result = NIL;
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(desc, i);

		result = lappend(result, makeColumnDef(NameStr(a->attname), a->atttypid, a->atttypmod, a->attcollation));
	}
	return result;
}

/* Find "column = expression" filters in the raw WHERE clause for the
 * generated table, so that diff partitions can be selected directly. */
static void
commit_hints(ParseState *pstate, Node *where, const char *refname, char **from, char **to)
{
	if (!where)
		return;
	if (IsA(where, BoolExpr) && ((BoolExpr *) where)->boolop == AND_EXPR)
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) where)->args)
			commit_hints(pstate, (Node *) lfirst(lc), refname, from, to);
		return;
	}
	if (IsA(where, A_Expr) && ((A_Expr *) where)->kind == AEXPR_OP &&
		list_length(((A_Expr *) where)->name) == 1 && !strcmp(strVal(linitial(((A_Expr *) where)->name)), "="))
	{
		A_Expr	   *op = (A_Expr *) where;
		Node	   *column = IsA(op->lexpr, ColumnRef) ? op->lexpr : IsA(op->rexpr, ColumnRef) ? op->rexpr : NULL;
		Node	   *value = column == op->lexpr ? op->rexpr : op->lexpr;
		ColumnRef  *ref = (ColumnRef *) column;
		const char *name;

		if (!column || column == value)
			return;
		if (list_length(ref->fields) == 2 && IsA(linitial(ref->fields), String) && strcmp(strVal(linitial(ref->fields)), refname))
			return;
		if (list_length(ref->fields) > 2 || !IsA(llast(ref->fields), String))
			return;
		name = strVal(llast(ref->fields));
		if (!strcmp(name, "to_commit") && !*to)
			*to = constant_text(pstate, value);
		else if (!strcmp(name, "from_commit") && !*from)
			*from = constant_text(pstate, value);
	}
}

/* Resolve the base table of a generated name within a schema or the search
 * path, returning its qualified logical name. */
static char *
generated_base(const RangeVar *rv, const char *base, bool optional)
{
	List	   *schemas = NIL;
	ListCell   *lc;

	if (rv->schemaname)
	{
		if (!OidIsValid(get_namespace_oid(rv->schemaname, true)))
			return NULL;
		schemas = list_make1(pstrdup(rv->schemaname));
	}
	else
	{
		List	   *path = fetch_search_path(false);
		ListCell   *plc;

		foreach(plc, path)
		{
			char	   *ns = get_namespace_name(lfirst_oid(plc));

			if (ns && !dg_system_namespace(ns) && strncmp(ns, "pg_", 3))
				schemas = lappend(schemas, ns);
		}
		if (schemas == NIL)
			schemas = list_make1(pstrdup("public"));
	}
	foreach(lc, schemas)
	{
		RangeVar   *candidate = makeRangeVar((char *) lfirst(lc), pstrdup(base), -1);
		Oid			relid = RangeVarGetRelidExtended(candidate, NoLock, RVR_MISSING_OK, NULL, NULL);

		if (OidIsValid(relid) && get_rel_relkind(relid) == RELKIND_RELATION)
			return dg_qualified_name((char *) lfirst(lc), base);
	}
	if (optional)
		return dg_qualified_name((char *) linitial(schemas), base);
	return NULL;
}

/* Per-schema tables generated from the catalog: dolt_ignore (when no real
 * table exists yet), dolt_schemas and dolt_procedures. */
static const struct
{
	const char *name;
	const char *function;
}			schema_tables[] = {
	{"dolt_ignore", "dolt_ignore_table"},
	{"dolt_schemas", "dolt_schemas_table"},
	{"dolt_procedures", "dolt_procedures_table"},
};

/* The schema a per-schema table name refers to: the explicit schema, which
 * must exist, or the first user schema on the search path. */
static char *
schema_table_namespace(const RangeVar *rv)
{
	List	   *path;
	ListCell   *lc;

	if (rv->schemaname)
	{
		if (!OidIsValid(get_namespace_oid(rv->schemaname, true)))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s.%s", rv->schemaname, rv->relname)));
		return pstrdup(rv->schemaname);
	}
	path = fetch_search_path(false);
	foreach(lc, path)
	{
		char	   *ns = get_namespace_name(lfirst_oid(lc));

		if (ns && !dg_system_namespace(ns) && strncmp(ns, "pg_", 3))
			return ns;
	}
	return pstrdup("public");
}

static RangeFunction *
schema_table(const RangeVar *rv)
{
	int			i;

	for (i = 0; i < lengthof(schema_tables); i++)
	{
		const char *schema;
		FuncCall   *fc;
		RangeFunction *rf;

		if (strcmp(rv->relname, schema_tables[i].name))
			continue;
		/* A real table of that name takes precedence. */
		if (OidIsValid(RangeVarGetRelidExtended((RangeVar *) rv, NoLock, RVR_MISSING_OK, NULL, NULL)))
			return NULL;
		schema = schema_table_namespace(rv);
		fc = makeFuncCall(list_make2(makeString("pg_catalog"), makeString(pstrdup(schema_tables[i].function))), list_make1(text_constant(schema)), COERCE_EXPLICIT_CALL, rv->location);
		rf = makeNode(RangeFunction);
		rf->functions = list_make1(list_make2(fc, NIL));
		rf->alias = rv->alias ? rv->alias : makeAlias(rv->relname, NIL);
		return rf;
	}
	return NULL;
}

/* INSERT INTO [schema.]dolt_ignore creates the schema's ignore table. */
/* TODO KNOWN-ISSUES.md: move lazy creation from analysis to execution. */
void
dg_system_insert_target(const RangeVar *rv)
{
	if (strcmp(rv->relname, "dolt_ignore") || rv->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (rv->schemaname && dg_system_namespace(rv->schemaname))
		return;
	if (OidIsValid(RangeVarGetRelidExtended((RangeVar *) rv, NoLock, RVR_MISSING_OK, NULL, NULL)))
		return;
	dg_ensure_ignore_table(schema_table_namespace(rv));
}

/* dolt.rebase (and its dolt_rebase aliases) only exists while the session
 * branch is rebasing, like Dolt's working-set table. */
static void
rebase_table_guard(const RangeVar *rv)
{
	Oid			relid,
				dolt_schema;

	if (strcmp(rv->relname, "rebase") && strcmp(rv->relname, "dolt_rebase"))
		return;
	if (!IsUnderPostmaster)
		return;					/* initdb defines the alias views */
	relid = RangeVarGetRelidExtended((RangeVar *) rv, NoLock, RVR_MISSING_OK, NULL, NULL);
	if (!OidIsValid(relid))
		return;
	dolt_schema = get_namespace_oid("dolt", true);
	if (relid != get_relname_relid("dolt_rebase", PG_CATALOG_NAMESPACE) && (!OidIsValid(dolt_schema) || relid != get_relname_relid("rebase", dolt_schema)))
		return;
	if (!dg_rebase_active())
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", rv->relname)));
}

/* Column layout of a query for dolt_query_diff: the query must be a single
 * SELECT, analyzed with the caller's search path. */
static TupleDesc
query_layout(const char *sql)
{
	List	   *raw = raw_parser(sql, RAW_PARSE_DEFAULT);
	Query	   *query;

	if (list_length(raw) != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("query must be a SELECT statement")));
	query = parse_analyze_fixedparams((RawStmt *) linitial(raw), sql, NULL, 0, NULL);
	if (query->commandType != CMD_SELECT || query->utilityStmt)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("query must be a SELECT statement")));
	return ExecCleanTypeFromTL(query->targetList);
}

static TupleDesc
query_diff_layout(const char *first, const char *second)
{
	TupleDesc	a = query_layout(first),
				b = query_layout(second),
				result = CreateTemplateTupleDesc(a->natts + b->natts + 1);
	int			i;

	for (i = 0; i < a->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(a, i);

		TupleDescInitEntry(result, i + 1, psprintf("from_%s", NameStr(att->attname)), att->atttypid, att->atttypmod, 0);
	}
	for (i = 0; i < b->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(b, i);

		TupleDescInitEntry(result, a->natts + i + 1, psprintf("to_%s", NameStr(att->attname)), att->atttypid, att->atttypmod, 0);
	}
	TupleDescInitEntry(result, result->natts, "diff_type", TEXTOID, -1, 0);
	return result;
}

RangeFunction *
dg_system_table(ParseState *pstate, RangeVar *rv)
{
	int			i;
	RangeFunction *per_schema;

	if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")) || rv->relpersistence == RELPERSISTENCE_TEMP)
		return NULL;
	rebase_table_guard(rv);
	if (rv->schemaname && dg_system_namespace(rv->schemaname))
		return NULL;
	per_schema = schema_table(rv);
	if (per_schema)
		return per_schema;
	for (i = 0; i < lengthof(generated_tables); i++)
	{
		const		GeneratedTable *g = &generated_tables[i];
		size_t		plen = strlen(g->prefix);
		const char *base;
		char	   *qualified,
				   *from = NULL,
				   *to = NULL;
		const char *refname;
		TupleDesc	desc = NULL;
		RangeFunction *rf;
		FuncCall   *fc;
		List	   *args;

		if (strncmp(rv->relname, g->prefix, plen) || !rv->relname[plen])
			continue;
		base = rv->relname + plen;
		qualified = generated_base(rv, base, g->optional);
		if (!qualified)
			return NULL;
		refname = rv->alias ? rv->alias->aliasname : rv->relname;
		if (g->hints)
			commit_hints(pstate, pstate->p_dolt_where, refname, &from, &to);
		if (!g->optional || OidIsValid(RangeVarGetRelidExtended(makeRangeVar((char *) strVal(linitial(stringToQualifiedNameList(qualified, NULL))), pstrdup(base), -1), NoLock, RVR_MISSING_OK, NULL, NULL)))
			desc = dg_diff_layout(g->kind, qualified, NULL, NULL);
		else
		{
			/* dolt_workspace_<unknown> is empty with the fixed columns only */
			desc = CreateTemplateTupleDesc(3);
			TupleDescInitEntry(desc, 1, "id", INT8OID, -1, 0);
			TupleDescInitEntry(desc, 2, "staged", BOOLOID, -1, 0);
			TupleDescInitEntry(desc, 3, "diff_type", TEXTOID, -1, 0);
		}
		args = list_make1(text_constant(qualified));
		if (g->hints)
			args = lappend(lappend(args, text_constant(from)), text_constant(to));
		fc = makeFuncCall(list_make2(makeString("pg_catalog"), makeString(pstrdup(g->function))), args, COERCE_EXPLICIT_CALL, rv->location);
		rf = makeNode(RangeFunction);
		rf->functions = list_make1(list_make2(fc, layout_columns(desc)));
		rf->alias = rv->alias ? rv->alias : makeAlias(rv->relname, NIL);
		rf->lateral = false;
		rf->ordinality = false;
		rf->is_rowsfrom = false;
		return rf;
	}
	return NULL;
}

/* DELETE FROM dolt_conflicts_<t> / dolt_constraint_violations_<t> clears the
 * table's pending-merge entries. Rewritten into a SELECT of the clearing
 * function; a WHERE clause is not supported. */
Query *
dg_system_delete(ParseState *pstate, DeleteStmt *stmt)
{
	RangeVar   *rv = stmt->relation;
	int			i;

	if (!rv || rv->dolt_revision || (rv->schemaname && dg_system_namespace(rv->schemaname)) || stmt->withClause || stmt->usingClause || stmt->returningClause)
		return NULL;
	for (i = 0; i < lengthof(deletable_tables); i++)
	{
		size_t		plen = strlen(deletable_tables[i].prefix);
		char	   *qualified;
		SelectStmt *select;
		ResTarget  *target;
		FuncCall   *fc;

		if (strncmp(rv->relname, deletable_tables[i].prefix, plen) || !rv->relname[plen])
			continue;
		qualified = generated_base(rv, rv->relname + plen, false);
		if (!qualified)
			return NULL;
		if (stmt->whereClause)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("DELETE FROM %s with a WHERE clause is not supported; delete all rows to clear the table", rv->relname)));
		fc = makeFuncCall(list_make2(makeString("pg_catalog"), makeString(pstrdup(deletable_tables[i].function))), list_make1(text_constant(qualified)), COERCE_EXPLICIT_CALL, rv->location);
		target = makeNode(ResTarget);
		target->val = (Node *) fc;
		target->location = rv->location;
		select = makeNode(SelectStmt);
		select->targetList = list_make1(target);
		return transformStmt(pstate, (Node *) select);
	}
	return NULL;
}

/* Column definitions for Dolt table functions whose layout depends on the
 * table named in their constant arguments. */
List *
dg_system_function_columns(ParseState *pstate, FuncCall *fc)
{
	const char *name;
	int			nargs = list_length(fc->args);
	char	  **args;
	int			i;
	TupleDesc	desc = NULL;

	if (fc->agg_star || fc->agg_distinct || fc->func_variadic || fc->over || fc->agg_order || fc->agg_filter)
		return NIL;
	if (list_length(fc->funcname) == 2 && strcmp(strVal(linitial(fc->funcname)), "pg_catalog"))
		return NIL;
	if (list_length(fc->funcname) > 2)
		return NIL;
	name = strVal(llast(fc->funcname));
	if (strcmp(name, "dolt_diff") && strcmp(name, "dolt_preview_merge_conflicts") && strcmp(name, "dolt_query_diff"))
		return NIL;
	if (!strcmp(name, "dolt_query_diff") && nargs != 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("function 'dolt_query_diff' expected 2 arguments, %d received", nargs)));
	if (!strcmp(name, "dolt_preview_merge_conflicts") && nargs != 3)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("function 'dolt_preview_merge_conflicts' expected 3 arguments, %d received", nargs)));
	args = palloc0(sizeof(char *) * Max(nargs, 1));
	for (i = 0; i < nargs; i++)
	{
		Node	   *raw = (Node *) list_nth(fc->args, i);

		if (IsA(raw, A_Const) && ((A_Const *) raw)->isnull)
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("Invalid argument to %s: NULL", name)));
		args[i] = constant_text(pstate, raw);
		if (!args[i])
			return NIL;
	}
	if (!strcmp(name, "dolt_diff"))
	{
		if (nargs == 3)
			desc = dg_diff_layout("diff", args[2], args[0], args[1]);
		else if (nargs == 2)
			desc = dg_diff_layout("diff", args[1], args[0], NULL);
	}
	else if (!strcmp(name, "dolt_query_diff"))
		desc = query_diff_layout(args[0], args[1]);
	else if (nargs == 3)
		desc = dg_diff_layout("conflicts", args[2], args[0], args[1]);
	return desc ? layout_columns(desc) : NIL;
}

/* User content has ordinary branch-aware storage. Built-in aliases only name
 * these tables; they do not own a second copy of their rows. */
bool		dg_internal_system_ddl;

void
dg_system_user_target(RangeVar *rv)
{
	const char *name = rv->relname;

	if (rv->schemaname && !strcmp(rv->schemaname, "dolt"))
	{
		if (!strcmp(name, "docs"))
			name = "dolt_docs";
		else if (!strcmp(name, "tests"))
			name = "dolt_tests";
		else if (!strcmp(name, "query_catalog"))
			name = "dolt_query_catalog";
	}
	if (strcmp(name, "dolt_docs") && strcmp(name, "dolt_tests") && strcmp(name, "dolt_query_catalog"))
		return;
	if (rv->dolt_revision || rv->catalogname)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("revision-qualified metadata writes are not supported")));
	/* The initializer inserts the default document through this same path.
	 * Its CREATE has completed, but the enclosing statement's catalog view
	 * need not yet contain it. Do not recursively initialize the table. */
	if (!dg_internal_system_ddl)
		dg_ensure_user_table(name);
	rv->schemaname = pstrdup("public");
	rv->relname = pstrdup(name);
}

static void
reserved_name(const char *name)
{
	if (name && pg_strncasecmp(name, "dolt", 4) == 0)
		ereport(ERROR, (errcode(ERRCODE_RESERVED_NAME), errmsg("name \"%s\" is reserved for Dolt", name)));
}

void
dg_system_check_ddl(Node *statement)
{
	if (!IsUnderPostmaster || dg_internal_system_ddl)
		return;
	if (IsA(statement, CreateStmt))
		reserved_name(((CreateStmt *) statement)->relation->relname);
	else if (IsA(statement, CreateForeignTableStmt))
		reserved_name(((CreateForeignTableStmt *) statement)->base.relation->relname);
	else if (IsA(statement, CompositeTypeStmt))
		reserved_name(((CompositeTypeStmt *) statement)->typevar->relname);
	else if (IsA(statement, CreateSchemaStmt))
	{
		CreateSchemaStmt *stmt = (CreateSchemaStmt *) statement;

		reserved_name(stmt->schemaname ? stmt->schemaname : get_rolespec_name(stmt->authrole));
	}
	else if (IsA(statement, ViewStmt))
		reserved_name(((ViewStmt *) statement)->view->relname);
	else if (IsA(statement, CreateTableAsStmt))
		reserved_name(((CreateTableAsStmt *) statement)->into->rel->relname);
	else if (IsA(statement, CreateSeqStmt))
		reserved_name(((CreateSeqStmt *) statement)->sequence->relname);
	else if (IsA(statement, RenameStmt))
	{
		RenameStmt *stmt = (RenameStmt *) statement;

		if (stmt->renameType == OBJECT_SCHEMA || stmt->renameType == OBJECT_TABLE ||
			stmt->renameType == OBJECT_VIEW || stmt->renameType == OBJECT_MATVIEW ||
			stmt->renameType == OBJECT_FOREIGN_TABLE || stmt->renameType == OBJECT_SEQUENCE || stmt->renameType == OBJECT_INDEX)
			reserved_name(stmt->newname);
	}
	else if (IsA(statement, IndexStmt) && !((IndexStmt *) statement)->isconstraint)
		reserved_name(((IndexStmt *) statement)->idxname);
}
