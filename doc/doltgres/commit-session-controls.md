# Commit session controls

These are native PostgreSQL GUCs. Boolean settings accept `0`/`1` and
`off`/`on`; all settings support PostgreSQL's transactional `SET`, `SET LOCAL`,
`RESET`, and session isolation.

| Setting | Default | Behavior |
| --- | --- | --- |
| `dolt_transaction_commit` | off | Create a version-control commit when a SQL transaction changes a branch. |
| `dolt_multi_branch_commit` | off | Allow automatic version commits on every dirty branch in one SQL transaction. |
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
With `dolt_multi_branch_commit=on`, automatic publication visits every dirty
branch in branch-name order, independently of the session's selected branch.
Data and all HEAD updates share one transaction decision. A failure rolls back
all branches; ordinary PostgreSQL serialization/deadlock errors require retrying
the entire transaction. PREPARE preserves the same all-or-nothing boundary.
Branches discarded by savepoint rollback are omitted. Pending merges remain
working-set-only updates while other branches can receive version commits.

Qualified INSERT, UPDATE and DELETE targets use the named branch's catalog,
constraints, defaults and triggers. Historical targets remain read-only.
Qualified `AS OF 'HEAD'` resolves HEAD on the named branch, not the session's.

`dolt_commit()` and `dolt_commit_all()` commit the current SQL transaction,
including when called inside BEGIN. Both require a standalone SELECT (or a
supported scalar wrapper), never a per-row or nested invocation. Commit finishes
through PostgreSQL's normal transaction path after delivering the result, before
CommandComplete, without waiting for protocol Sync. A commit command delivers its
whole result even if an extended-protocol Execute requests a limited fetch.

`dolt_commit_all()` returns `(branch, hash)` rows in branch-name order for every
dirty branch. `dolt_multi_branch_commit=on` makes `dolt_commit()` use the same set,
returning only hash rows. Otherwise `dolt_commit()` rejects transactions with more
than one dirty branch. With no dirty branches, the selected branch is used.
`--skip-empty` returns no rows for unchanged branches; `--allow-empty` creates
commits even there. Staged-only commits preserve unstaged working data and do not
trigger an extra automatic version commit.

Deferred constraints and holdable-cursor execution finish before root capture.
Ref publication remains atomic: an error preparing any branch aborts the SQL
transaction and leaves all HEADs unchanged. Native failed-transaction recovery
requires ROLLBACK (or a suitable savepoint), unlike Dolt's retry-in-place behavior.
Existing unsupported commit options such as `--amend` remain unsupported.
