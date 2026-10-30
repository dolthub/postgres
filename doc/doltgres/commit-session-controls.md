# Commit session controls

These are native PostgreSQL GUCs. Boolean settings accept `0`/`1` and
`off`/`on`; all settings support PostgreSQL's transactional `SET`, `SET LOCAL`,
`RESET`, and session isolation.

| Setting | Default | Behavior |
| --- | --- | --- |
| `dolt_transaction_commit` | off | Create a version-control commit when a SQL transaction changes a single branch. |
| `dolt_transaction_commit_message` | empty | Automatic commit message; empty/space-only uses `Transaction commit`. |
| `dolt_force_transaction_commit` | off | Retain foreign-key violations produced by a merge, recording offending rows for repair. |
| `dolt_allow_commit_conflicts` | off | Accepted for compatibility. Both values preserve the fork's non-aborting merge conflict contract. |
| `dolt_author_name`, `dolt_author_email`, `dolt_author_date` | empty | Override author metadata. |
| `dolt_committer_name`, `dolt_committer_email`, `dolt_committer_date` | empty | Override committer metadata independently. |

Empty identity fields fall back independently to the SQL role/client identity.
Explicit `--author`, `--committer`, and `--date` arguments take precedence over
corresponding defaults. An explicitly supplied author also supplies the committer
unless the committer was independently overridden. An explicit date supplies the
committer date when there is no committer-date override. Invalid dates fail the
commit without moving refs; a failed automatic commit also rolls back the SQL
transaction's data changes.

Automatic commits track branch changes per SQL transaction and savepoint. Reads,
SET commands, rolled-back writes and no-op changes do not create commits. An
explicit version-control commit does not create an additional automatic commit.
Automatic capture follows storage-root publication/merging so HEAD includes the
published working state. The objects, refs, schemas and row changes participate
in the same PostgreSQL transaction, including PREPARE / COMMIT PREPARED and
ROLLBACK PREPARED. Pending merges remain explicit: automatic commits do not
finalize a pending merge or discard its conflicts/violation records.

The force setting does **not** disable PostgreSQL's normal DML constraints.
Forced merge violations appear in `dolt_constraint_violations` and per-table
violation tables. Repair the underlying rows, clear their violation records, and
commit the repaired merge. Explicit version commits still reject unresolved
conflicts or recorded constraint violations. Existing non-aborting reporting of
unique/check merge violations is unchanged.

`dolt_transactions_disabled` deliberately does not exist: SET rejects both 0
and 1. PostgreSQL transaction machinery is never disabled.
`dolt_multi_branch_commit` remains deferred; automatic publication rejects a
transaction that has changed multiple branches. No compatibility setting is
registered for that deferred feature.
