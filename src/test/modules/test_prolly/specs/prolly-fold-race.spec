# Two transactions change rows the first commits after the second's
# snapshot: the second must not apply the first's changes again, whether it
# reads through the table or through an index. (Written for the removed row
# journal, whose folds raced this way; kept for the delta store's folds.)

setup
{
	CREATE TABLE fr(id int PRIMARY KEY, v int) USING dolt_prolly_table;
	CREATE INDEX fr_v ON fr(v);
	INSERT INTO fr SELECT g, g FROM generate_series(1, 200) g;
	UPDATE fr SET v = v + 1000 WHERE id = 1;
}

teardown
{
	DROP TABLE fr;
}

session s1
setup		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_read	{ SELECT count(*), sum(v) FROM fr; }
step s1_fold	{ UPDATE fr SET v = v + 1 WHERE id BETWEEN 51 AND 100; }
step s1_point	{ SELECT v FROM fr WHERE id = 1; }
step s1_index	{ SET enable_seqscan = off; SELECT id FROM fr WHERE v = 1001; }
step s1_scan	{ SELECT count(*), sum(v) FROM fr; }
step s1_c	{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_fold	{ UPDATE fr SET v = v + 1 WHERE id > 150; }
step s2_c	{ COMMIT; }
step s2_check	{ SELECT count(*), sum(v) FROM fr; }

# The first to fold commits first: the other folded nothing it still sees.
permutation s1_read s2_fold s1_fold s2_c s1_point s1_index s1_scan s1_c s2_check
permutation s1_read s2_fold s2_c s1_fold s1_point s1_index s1_scan s1_c s2_check
# The reader folds first, the other folds the same entries and commits.
permutation s1_read s1_fold s2_fold s2_c s1_point s1_index s1_scan s1_c s2_check
