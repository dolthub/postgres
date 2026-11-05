/*-------------------------------------------------------------------------
 *
 * doltdelta.h
 *	  The prolly delta store: committed and pending changes over each tree's base.
 *
 * See src/backend/access/dolt_prolly/dg_delta.c.
 *
 * src/include/access/doltdelta.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTDELTA_H
#define DOLTDELTA_H

#include "access/doltdelta_xlog.h"
#include "access/xlogdefs.h"
#include "utils/snapshot.h"

/* One prolly tree: a storage on a branch, in a database. */
typedef struct DoltDeltaTree
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	char		branch[DOLT_DELTA_BRANCH_SIZE];
} DoltDeltaTree;

/* A change to one key: |delta| more occurrences of |row|. */
typedef struct DoltDeltaEdit
{
	const uint8 *key;
	uint32		keylen;
	const uint8 *row;
	uint32		rowlen;
	int64		delta;
} DoltDeltaEdit;

/*
 * What a base holds of its tree's delta: every version its fold's snapshot
 * saw, which commits by top-level xid name. A horizon with an invalid xmax
 * holds none: the base was written whole, not folded.
 */
typedef struct DoltDeltaHorizon
{
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	TransactionId *xip;
} DoltDeltaHorizon;

/*
 * What one reader sees of a tree's delta: the versions |snapshot| sees, but
 * not those |base| holds, and this transaction's own only if |own|.
 */
typedef struct DoltDeltaView
{
	DoltDeltaTree tree;
	Snapshot	snapshot;
	const DoltDeltaHorizon *base;
	bool		own;
} DoltDeltaView;

/* A visited key: its net change, the row its key names. Pointers are borrowed. */
typedef void (*DoltDeltaVisit) (void *arg, const DoltDeltaEdit *edit);

extern PGDLLIMPORT int dolt_delta_memory;

extern Size DoltDeltaShmemSize(void);
extern void DoltDeltaShmemInit(void);
extern void CheckPointDoltDelta(XLogRecPtr redo);
extern void StartupDoltDelta(XLogRecPtr redo);

extern void dolt_delta_tree_key(DoltDeltaTree *tree, Oid db, Oid space, RelFileNumber file, const char *branch);
extern void dolt_delta_append(const DoltDeltaTree *tree, const DoltDeltaEdit *edits, int count,
							  CommandId cid, bool logged);
extern bool dolt_delta_get(const DoltDeltaView *view, const uint8 *key, uint32 keylen,
						   DoltDeltaVisit visit, void *arg);
extern int	dolt_delta_range(const DoltDeltaView *view, const uint8 *from, uint32 fromlen, bool inclusive,
							 const uint8 *to, uint32 tolen, bool backward, int max,
							 DoltDeltaVisit visit, void *arg);
extern uint64 dolt_delta_seq(const DoltDeltaTree *tree);
extern int64 dolt_delta_net(const DoltDeltaTree *tree);
extern uint64 dolt_delta_count(const DoltDeltaTree *tree);

extern void dolt_delta_pin(TransactionId xmin);
extern void dolt_delta_unpin(void);
extern void dolt_delta_set_fold(const DoltDeltaTree *tree, TransactionId fold, const DoltDeltaHorizon *horizon,
								const uint8 *root, const uint8 *base, const DoltDeltaHorizon *base_horizon);

/* Rebuild a fold's tree in recovery, if the chunk store lacks it (dg_store.c). */
extern void dg_refold(const DoltDeltaTree *tree, const DoltDeltaHorizon *horizon, const uint8 *root,
					  const uint8 *base, const DoltDeltaHorizon *base_horizon);
extern uint64 dolt_delta_prune(const DoltDeltaTree *tree);
extern uint64 dolt_delta_prune_all(bool all);
extern int	dolt_delta_due(Oid db, int versions, int timeout_ms, DoltDeltaTree *due, int max);
extern void dolt_delta_totals(uint64 *ntrees, uint64 *keys, uint64 *versions, uint64 *bytes);

/* Background folding, dg_fold.c */
extern PGDLLIMPORT int dolt_fold_workers;
extern PGDLLIMPORT int dolt_fold_versions;
extern PGDLLIMPORT int dolt_fold_batch;
extern PGDLLIMPORT bool dolt_unlogged_folds;
extern PGDLLIMPORT int dolt_fold_timeout;

extern void DoltFoldLauncherRegister(void);
extern void DoltFoldLauncherMain(Datum argument);
extern void DoltFoldWorkerMain(Datum argument);

#endif							/* DOLTDELTA_H */
