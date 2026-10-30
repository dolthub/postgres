/* Session controls for version-control publication and commit metadata. */
#ifndef DOLTVERSION_H
#define DOLTVERSION_H
#include "postgres.h"
extern PGDLLIMPORT bool dolt_transaction_commit;
extern PGDLLIMPORT bool dolt_force_transaction_commit;
extern PGDLLIMPORT bool dolt_allow_commit_conflicts;
extern PGDLLIMPORT char *dolt_transaction_commit_message;
extern PGDLLIMPORT char *dolt_author_name;
extern PGDLLIMPORT char *dolt_author_email;
extern PGDLLIMPORT char *dolt_author_date;
extern PGDLLIMPORT char *dolt_committer_name;
extern PGDLLIMPORT char *dolt_committer_email;
extern PGDLLIMPORT char *dolt_committer_date;
extern void dg_version_mark_branch(const char *branch);
extern void dg_version_pre_commit(void);
extern void dg_version_pre_prepare(void);
#endif
