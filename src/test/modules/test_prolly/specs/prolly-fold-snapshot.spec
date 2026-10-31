# A transaction whose write folds a prolly table's journal into roots it saves
# must go on reading its own snapshot: another session's commit after that
# snapshot stays invisible to it, the versions it saw stay fetchable, and
# updating a row the other session changed is a serialization failure.

setup
{
	CREATE TABLE fs(id int PRIMARY KEY, v int) USING dolt_prolly_table;
	INSERT INTO fs SELECT g, g FROM generate_series(1, 200) g;
}

teardown
{
	DROP TABLE fs;
}

session s1
setup		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_read	{ SELECT count(*), sum(v) FROM fs; }
step s1_fold	{ UPDATE fs SET v = v + 1 WHERE id > 100; }
step s1_point	{ SELECT v FROM fs WHERE id = 1; }
step s1_scan	{ SELECT count(*), sum(v) FROM fs; }
step s1_upd	{ UPDATE fs SET v = 0 WHERE id = 1; }
step s1_upd_own	{ UPDATE fs SET v = v * 2 WHERE id = 150; }
step s1_own	{ SELECT v FROM fs WHERE id = 150; }
step s1_c	{ COMMIT; }

session s2
step s2_upd	{ UPDATE fs SET v = v + 1000 WHERE id = 1; }
step s2_check	{ SELECT v FROM fs WHERE id IN (1, 150) ORDER BY id; }

# The other session commits after the snapshot, then the fold.
permutation s1_read s2_upd s1_fold s1_point s1_scan s1_upd s1_c s2_check
# Updates after the fold read the folded rows and the session's own writes.
permutation s1_read s2_upd s1_fold s1_upd_own s1_own s1_scan s1_c s2_check
