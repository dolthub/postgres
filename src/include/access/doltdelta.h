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

/* A visited version: its edit, and its sequence in the tree's delta. */
typedef void (*DoltDeltaVisit) (void *arg, const DoltDeltaEdit *edit, uint64 seq);

extern PGDLLIMPORT int dolt_delta_memory;

extern Size DoltDeltaShmemSize(void);
extern void DoltDeltaShmemInit(void);
extern void CheckPointDoltDelta(XLogRecPtr redo);
extern void StartupDoltDelta(XLogRecPtr redo);

extern void dolt_delta_tree_key(DoltDeltaTree *tree, Oid db, Oid space, RelFileNumber file, const char *branch);
extern void dolt_delta_append(const DoltDeltaTree *tree, const DoltDeltaEdit *edits, int count, bool logged);
extern int	dolt_delta_scan(const DoltDeltaTree *tree, Snapshot snapshot, const DoltDeltaHorizon *base,
							DoltDeltaVisit visit, void *arg);
extern uint64 dolt_delta_count(const DoltDeltaTree *tree);

#endif							/* DOLTDELTA_H */
