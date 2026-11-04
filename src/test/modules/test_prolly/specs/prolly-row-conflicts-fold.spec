# prolly-row-conflicts after many updates to the hot rows, as TPC-C's district
# rows see. The outcomes are the same as without them.

setup
{
	CREATE TABLE hot(id int PRIMARY KEY, next int, ytd int) USING dolt_prolly_table;
	INSERT INTO hot SELECT g, 1, 0 FROM generate_series(1, 10) g;
	DO $$ BEGIN FOR i IN 1..127 LOOP UPDATE hot SET ytd = ytd + 1 WHERE id = 3 + i % 8; END LOOP; END $$;
}

teardown
{
	DROP TABLE hot;
}

session s1
step s1_rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1_lock	{ SELECT next FROM hot WHERE id = 1 FOR UPDATE; }
step s1_next	{ UPDATE hot SET next = next + 1 WHERE id = 1; }
step s1_ytd	{ UPDATE hot SET ytd = ytd + 10 WHERE id = 1; }
step s1_c	{ COMMIT; }

session s2
step s2_rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2_read	{ SELECT count(*) FROM hot; }
step s2_lock	{ SELECT next FROM hot WHERE id = 1 FOR UPDATE; }
step s2_next	{ UPDATE hot SET next = next + 1 WHERE id = 1; }
step s2_ytd	{ UPDATE hot SET ytd = ytd + 1 WHERE id = 1; }
step s2_other	{ UPDATE hot SET ytd = ytd + 1 WHERE id = 2; }
step s2_c	{ COMMIT; }

session s3
step s3_check	{ SELECT id, next, ytd FROM hot WHERE id IN (1, 2) ORDER BY id; }
step s3_rest	{ SELECT sum(ytd) FROM hot WHERE id > 2; }

# New-order's district lock: the second locker waits, then fails under repeatable read.
permutation s1_rr s2_rr s1_lock s2_lock s1_next s1_c s2_c s3_check s3_rest
# Under read committed it waits, then locks and increments the newest version.
permutation s1_rc s2_rc s1_lock s2_lock s1_next s1_c s2_next s2_c s3_check s3_rest
# Payment's district update: the second updater waits, then fails under repeatable read.
permutation s1_rr s2_rr s1_ytd s2_ytd s1_c s2_c s3_check s3_rest
# Under read committed both increments land.
permutation s1_rc s2_rc s1_ytd s2_ytd s1_c s2_c s3_check s3_rest
# A change committed after the snapshot fails the update without any wait.
permutation s2_rr s2_read s1_rr s1_ytd s1_c s2_ytd s2_c s3_check s3_rest
# Another row is no conflict.
permutation s1_rr s2_rr s1_ytd s2_other s1_c s2_c s3_check s3_rest
# Repeated: lock, update, commit; then the other session locks the newest version.
permutation s1_rr s1_lock s1_next s1_ytd s1_c s2_rr s2_lock s2_next s2_ytd s2_c s3_check s3_rest
