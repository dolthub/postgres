/* Dolt system table and namespace resolution hooks. */
#ifndef DOLT_SYSTEM_H
#define DOLT_SYSTEM_H
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "access/tupdesc.h"

extern PGDLLIMPORT bool dolt_show_system_tables;

struct ParseState;
struct RangeFunction;
struct FuncCall;
extern Oid	dg_system_relation(const RangeVar *rv);
extern bool dg_system_namespace(const char *name);
extern struct RangeFunction *dg_system_table(struct ParseState *pstate, RangeVar *rv);
extern List *dg_system_function_columns(struct ParseState *pstate, struct FuncCall *fc);
extern Query *dg_system_delete(struct ParseState *pstate, DeleteStmt *stmt);
extern void dg_system_insert_target(const RangeVar *rv);
extern void dg_system_user_target(RangeVar *rv);
extern void dg_system_check_ddl(Node *statement);
extern void dg_system_function_context(FuncExpr *expr, const char *database, const char *branch);
extern void dg_ensure_user_table(const char *name);
extern bool dg_internal_system_ddl;
extern void dg_ensure_ignore_table(const char *schema);
extern bool dg_rebase_active(void);
extern TupleDesc dg_diff_layout(const char *kind, const char *table, const char *from, const char *to);
#endif
