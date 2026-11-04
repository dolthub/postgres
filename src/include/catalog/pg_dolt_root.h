/*-------------------------------------------------------------------------
 *
 * pg_dolt_root.h
 *	  Committed working roots of prolly relations, per branch.
 *
 * One row per (relation storage, branch) holds the base of that relation's
 * content tree in the chunk store: the tree its delta store versions apply
 * to (access/doltdelta.h). A fold replaces the row with a tree holding every
 * version its snapshot saw, recorded as the row's horizon, so ordinary MVCC
 * selects the base a snapshot sees, and the horizon which versions to add.
 * A row written whole, not folded, has no horizon (rootxmax invalid).
 * Storage is identified by tablespace and file number, so TRUNCATE (a new
 * file) starts from empty roots and rolling it back restores the old ones.
 * Branch names are authoritative; a rename rewrites them.
 *
 * src/include/catalog/pg_dolt_root.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_ROOT_H
#define PG_DOLT_ROOT_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_root_d.h"

CATALOG(pg_dolt_root,9127,DoltRootRelationId)
{
	Oid			rootspace BKI_FORCE_NOT_NULL;	/* tablespace of the storage */
	Oid			rootfile BKI_FORCE_NOT_NULL;	/* file number of the storage */
	TransactionId rootxmin BKI_FORCE_NOT_NULL;	/* the folding snapshot's xmin */
	TransactionId rootxmax BKI_FORCE_NOT_NULL;	/* its xmax; invalid if not folded */
#ifdef CATALOG_VARLEN
	text		rootbranch BKI_FORCE_NOT_NULL;
	bytea		rootcontent BKI_FORCE_NOT_NULL;
	TransactionId rootxip[1] BKI_FORCE_NULL;	/* its running top-level xids */
#endif
} FormData_pg_dolt_root;

typedef FormData_pg_dolt_root *Form_pg_dolt_root;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_root_key_index, 9128, DoltRootKeyIndexId, pg_dolt_root, btree(rootspace oid_ops, rootfile oid_ops, rootbranch text_ops));

#endif							/* PG_DOLT_ROOT_H */
