/* Read-only versioned catalog provider. No persistent projection objects. */
#ifndef DOLT_CATALOG_H
#define DOLT_CATALOG_H
#include "access/htup.h"
#include "access/tupdesc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/extensible.h"
#include "storage/relfilelocator.h"
struct ParseState;
struct CatalogView;
extern bool dg_catalog_active(void);
extern void dg_roots_published(void);
extern bool dg_catalog_refs_current(List *refs);
extern void dg_catalog_session_dependency(List **refs);
extern bool dg_catalog_schema_object_ddl(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_routine_ddl(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_trigger_ddl(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_sequence_retained(Relation rel, int64 *last, bool *called);
extern void dg_catalog_sequence_check(Relation rel);
extern void dg_catalog_sequence_refresh(Relation rel);
extern bool dg_catalog_sequence_ddl(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_type_ddl(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_rename(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_view_ddl(struct ParseState *pstate, Node *statement, int location, int length);
extern List *dg_rule_actions(Oid relid, List *actions);
extern Node *dg_routine_body(HeapTuple proctup, Node *body);
extern bool dg_catalog_create_matview(struct CreateStmt *create, struct Query *query);
extern void dg_catalog_matview_refresh(Relation rel, bool populated);
extern bool dg_catalog_creating_routine(void);
extern Oid	dg_catalog_routine_namespace(Oid namespaceid);
extern Oid	dg_catalog_store_routine(HeapTuple tuple, TupleDesc desc);
extern struct CatalogView *dg_catalog_session_view(void);
extern bool dg_catalog_lookup_type(const char *schema, const char *name, bool temp_ok, Oid *result);
extern bool dg_catalog_type_visible(Oid id, const char *name, bool *visible);
extern bool dg_catalog_relation_visible(Oid id, const char *name, bool *visible);
extern void dg_catalog_rewrite_relation(RangeTblEntry *rte);
extern bool dg_catalog_alter(Node *statement);
extern Relation dg_catalog_trigger_relation(Oid id, const char *branch);
extern void dg_catalog_foreign_lock(Relation definition);
extern bool dg_catalog_lock_relation(Oid id, LOCKMODE mode, bool nowait);
extern bool dg_catalog_lookup_relation(const RangeVar *rv, bool missing_ok, Oid *result);
extern Relation dg_catalog_target(struct ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte);
extern void dg_storage_close_relation(Relation rel);
extern void dg_truncate(Relation rel);
extern bool dg_tuple_inserted_by_self(Relation rel, ItemPointer tid);
extern Relation dg_storage_index(Relation rel, Oid id);
extern struct CatalogView *dg_catalog_view(const char *database, const char *branch);
extern bool dg_catalog_create(Node *statement, const char *query);
extern void dg_catalog_schema_created(Oid schema);
extern bool dg_catalog_drop(Node *statement);
extern bool dg_catalog_grant(Node *statement);
extern void dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot);
extern TupleDesc dg_catalog_bind(struct ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte);
extern void dg_catalog_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
extern const CustomScanMethods *dg_catalog_scan_methods(void);
extern void dg_catalog_permission(const char *descriptor, Oid user);
extern void dg_storage_locator(Oid resource, RelFileLocator *locator);
extern struct CatalogView *dg_catalog_resolve_durable(Oid oid);
extern Oid	dg_binding_identity(Relation binding, HeapTuple tuple);
extern Oid	dg_identity_storage(Oid oid);
extern bytea *dg_catalog_durable_content(Oid oid);
extern bytea *dg_catalog_revision_content(Oid handle);
extern bool dg_relation_analyzable(Relation rel);
extern void dg_store_relation_stats(Oid relid, double rows);
extern void dg_remove_relation_stats(Oid relid);
extern List *dg_identity_relations(const char *branch);
extern int	dg_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
								   double *totalrows, double *totaldeadrows);
extern bool dg_catalog_provides(Oid catalog);
extern bool dg_storage_bound(Relation rel);
#endif
