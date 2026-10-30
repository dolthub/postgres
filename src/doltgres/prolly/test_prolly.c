#include "internal.h"
#include <stdio.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)
#define OK(expression) CHECK((expression) == PL_OK)

static bool file_mode;

static PLStore *
test_store_create(void)
{
	PLStore    *store = NULL;
	char		path[] = "/tmp/prolly-suite-XXXXXX";
	int			fd;

	if (!file_mode)
		return pl_store_create();
	fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(close(fd) == 0);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));

	/*
	 * These fixtures need no pathname after open; unlink prevents test
	 * litter.
	 */
	CHECK(unlink(path) == 0);
	return store;
}

static PLSlice
bytes(const void *data, size_t length)
{
	PLSlice		result = {data, length};

	return result;
}

static PLSlice
slice(const char *s)
{
	PLSlice		result = {(const uint8_t *) s, strlen(s)};

	return result;
}

static void
test_tuples(void)
{
	PLValue		fields[] = {{true, {NULL, 0}}, {false, {NULL, 0}}, {true, {NULL, 0}}};
	uint8_t		key[8],
			   *data = NULL;
	size_t		size,
				i;
	PLValue		value;
	PLSlice		tuple;

	pl_key_int64(-123, key);
	fields[0].bytes = (PLSlice)
	{
		key, 8
	};
	OK(pl_tuple_pack(fields, 3, &data, &size));
	tuple = (PLSlice)
	{
		data, size
	};
	OK(pl_tuple_get(tuple, 0, &value));
	CHECK(value.present && pl_compare(value.bytes, fields[0].bytes) == 0);
	OK(pl_tuple_get(tuple, 1, &value));
	CHECK(!value.present);
	OK(pl_tuple_get(tuple, 2, &value));
	CHECK(value.present && !value.bytes.len);
	for (i = 0; i < size; i++)
		CHECK(pl_tuple_get(bytes(data, i), 0, &value) == PL_CORRUPT);
	CHECK(pl_tuple_get(tuple, 3, &value) == PL_NOT_FOUND);
	data[12] |= 128;
	CHECK(pl_tuple_get(tuple, 0, &value) == PL_CORRUPT);
	free(data);
}

/* Bulk decoding validates every field, including fields never requested. */
static void
test_tuple_view(void)
{
	PLValue fields[64], value;
	PLTupleView view;
	uint8_t *data;
	size_t size;

	for (size_t i = 0; i < 64; i++)
		fields[i] = (PLValue) {i % 3 != 0, slice(i % 2 ? "" : "payload")};
	OK(pl_tuple_pack(fields, 64, &data, &size));
	OK(pl_tuple_open(bytes(data, size), &view));
	CHECK(view.count == 64);
	for (size_t i = 0; i < 64; i++)
	{
		OK(pl_tuple_field(&view, i, &value));
		CHECK(value.present == fields[i].present);
		if (value.present)
			CHECK(pl_compare(value.bytes, fields[i].bytes) == 0);
	}
	CHECK(pl_tuple_field(&view, 64, &value) == PL_NOT_FOUND);
	for (size_t i = 0; i < size; i++)
		CHECK(pl_tuple_open(bytes(data, i), &view) == PL_CORRUPT);
	/* A corrupt offset late in the row fails the fields it bounds, when read. */
	data[12 + 8 + 4 * 63] = 255;
	OK(pl_tuple_open(bytes(data, size), &view));
	OK(pl_tuple_get(bytes(data, size), 0, &value));
	CHECK(pl_tuple_get(bytes(data, size), 62, &value) == PL_CORRUPT);
	CHECK(pl_tuple_get(bytes(data, size), 63, &value) == PL_CORRUPT);
	free(data);
	OK(pl_tuple_pack(NULL, 0, &data, &size));
	OK(pl_tuple_open(bytes(data, size), &view));
	CHECK(pl_tuple_field(&view, 0, &value) == PL_NOT_FOUND);
	free(data);
}

static void
test_tree(void)
{
	PLStore    *store = test_store_create();
	PLTree		tree,
				branch,
				fresh;
	PLEntry    *entries = calloc(5000, sizeof(*entries));
	uint8_t		(*keys)[8] = calloc(5000, 8);
	PLValue		value;
	PLEdit		edit;
	PLStats		before,
				after;
	size_t		i;

	CHECK(store);
	CHECK(entries);
	CHECK(keys);
	for (i = 0; i < 5000; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i] = (PLEntry)
		{
			{
				keys[i], 8
			}, slice("encoded tuple bytes")
		};
	}
	OK(pl_build(store, entries, 5000, &tree));
	OK(pl_validate(&tree));
	CHECK(pl_count(&tree) == 5000);
	branch = pl_branch(&tree);
	CHECK(pl_hash_equal(branch.root, tree.root));
	for (i = 0; i < 5000; i += 37)
	{
		OK(pl_get(&tree, entries[i].key, &value));
		CHECK(value.present && pl_compare(value.bytes, entries[i].value) == 0);
	}
	edit = (PLEdit)
	{
		entries[2500].key,
		{
			true, slice("changed")
		}
	};
	before = pl_store_stats(store);
	OK(pl_apply(&branch, &edit, 1, &branch));
	after = pl_store_stats(store);
	/* One edit rewrites one root-to-leaf path and reuses every other subtree. */
	CHECK(after.skipped > before.skipped);
	CHECK(after.chunks - before.chunks < 8);
	OK(pl_get(&tree, edit.key, &value));
	CHECK(pl_compare(value.bytes, entries[2500].value) == 0);
	entries[2500].value = slice("changed");
	OK(pl_build(store, entries, 5000, &fresh));
	CHECK(pl_hash_equal(fresh.root, branch.root));
	OK(pl_validate(&branch));
	printf("localized edit: %zu new chunks; %" PRIu64 " reused subtrees\n",
		   after.chunks - before.chunks, after.skipped - before.skipped);
	free(keys);
	free(entries);
	pl_store_free(store);
}

/* A child reference is a 20-byte address prefix and a varint row count. */
static void
test_child_references(void)
{
	const uint64_t counts[] = {1, 127, 128, 16383, 16384, UINT64_C(1) << 35, UINT64_MAX};
	uint8_t		encoded[PL_CHILD_MAX + 1];
	PLHash		hash,
				decoded;
	uint64_t	rows;

	for (int i = 0; i < PL_HASH_SIZE; i++)
		hash.bytes[i] = (uint8_t) (i + 1);
	for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++)
	{
		size_t		length = pl_child_encode(encoded, hash, counts[c]);

		CHECK(length > PL_HASH_SIZE && length <= PL_CHILD_MAX);
		CHECK(pl_child_decode(bytes(encoded, length), &decoded, &rows));
		CHECK(rows == counts[c]);
		CHECK(memcmp(decoded.bytes, hash.bytes, PL_HASH_SIZE) == 0);
		for (int i = PL_HASH_SIZE; i < PL_HASH_SIZE; i++)
			CHECK(decoded.bytes[i] == 0);
		/* Truncated or trailing bytes are rejected. */
		CHECK(!pl_child_decode(bytes(encoded, length - 1), NULL, &rows));
		encoded[length] = 0;
		CHECK(!pl_child_decode(bytes(encoded, length + 1), NULL, &rows) || length + 1 > PL_CHILD_MAX);
	}
	CHECK(pl_child_encode(encoded, hash, 100) == PL_HASH_SIZE + 1);
	CHECK(pl_child_encode(encoded, hash, 5000) == PL_HASH_SIZE + 2);
	CHECK(!pl_child_decode(bytes(encoded, PL_HASH_SIZE), NULL, &rows));
}

/* Node sizes and counts by level: leaves below, interior nodes from 1 up. */
typedef struct LevelSizes
{
	size_t		nodes[2],
				bytes[2],
				largest[2];
	uint32_t	most_entries;
} LevelSizes;

static void
measure(PLStore *store, PLHash hash, LevelSizes *sizes)
{
	PLNode	   *node = pl_find(store, hash);
	int			interior;

	CHECK(node);
	interior = node->level > 0;
	/* Ordinary nodes use two-byte directory offsets. */
	CHECK(node->offset_width == 2);
	CHECK(node->size == PL_HEADER + 2 * 2 * (node->count + 1) + node->key_bytes + node->value_bytes);
	sizes->nodes[interior]++;
	sizes->bytes[interior] += node->size;
	if (node->size > sizes->largest[interior])
		sizes->largest[interior] = node->size;
	if (interior && node->count > sizes->most_entries)
		sizes->most_entries = node->count;
	for (uint32_t i = 0; node->level && i < node->count; i++)
	{
		PLHash		child;

		PLHash		decoded;
		uint64_t	rows;

		child = pl_child_hash(node, i);
		/* The unchecked reads agree with the validating decoder. */
		CHECK(pl_child_decode(pl_node_value(node, i), &decoded, &rows));
		CHECK(pl_hash_equal(child, decoded) && rows == pl_child_rows(node, i));
		measure(store, child, sizes);
	}
}

/* Leaves and interior nodes average about the target size and stay within the maximum. */
static void
test_node_sizes(void)
{
	PLStore    *store = test_store_create();
	size_t		n = 200000;
	PLEntry    *entries = calloc(n, sizeof(*entries));
	uint8_t		(*keys)[8] = calloc(n, 8);
	LevelSizes	sizes = {0};
	PLTree		tree;
	double		leaf,
				interior;

	CHECK(store && entries && keys);
	for (size_t i = 0; i < n; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i] = (PLEntry) {{keys[i], 8}, slice("a value of some forty bytes for each row")};
	}
	OK(pl_build(store, entries, n, &tree));
	OK(pl_validate(&tree));
	measure(store, tree.root, &sizes);
	leaf = (double) sizes.bytes[0] / sizes.nodes[0];
	interior = (double) sizes.bytes[1] / sizes.nodes[1];
	printf("node sizes: %zu leaves of %.0f bytes on average, %zu interior nodes of %.0f, largest %zu\n",
		   sizes.nodes[0], leaf, sizes.nodes[1], interior, sizes.largest[1]);
	CHECK(leaf > PL_TARGET_BYTES / 2 && leaf < 2 * PL_TARGET_BYTES);
	CHECK(interior > PL_TARGET_BYTES / 2 && interior < 2 * PL_TARGET_BYTES);
	CHECK(sizes.largest[0] <= PL_MAX_BYTES + PL_HEADER + 64);
	CHECK(sizes.largest[1] <= PL_MAX_BYTES + PL_HEADER + 64);
	CHECK(sizes.nodes[1] > 1);
	free(keys);
	free(entries);
	pl_store_free(store);
}

typedef struct DiffResult
{
	unsigned	inserts,
				removes,
				updates;
	PLSlice		previous;
	bool		have_previous;
} DiffResult;

static PLStatus
count_diff(void *arg, PLSlice key, PLValue before, PLValue after)
{
	DiffResult *result = arg;

	CHECK(!result->have_previous || pl_compare(result->previous, key) < 0);
	result->previous = key;
	result->have_previous = true;
	if (!before.present)
		result->inserts++;
	else if (!after.present)
		result->removes++;
	else
		result->updates++;
	return PL_OK;
}

static PLStatus
choose_right(void *arg, PLSlice key, PLValue base, PLValue left, PLValue right, PLValue *out)
{
	unsigned   *calls = arg;

	(void) key;
	(void) base;
	(void) left;
	(*calls)++;
	*out = right;
	return PL_OK;
}

static void
test_versions(void)
{
	PLStore    *store = test_store_create();
	PLTree		base,
				left,
				right,
				merged,
				expected,
				unchanged;
	PLEntry		entries[] = {{slice("a"), slice("one")}, {slice("b"), slice("two")},
	{slice("c"), slice("three")}};
	PLEdit		le[] = {{slice("a"), {true, slice("ONE")}}, {slice("d"), {true, slice("four")}}};
	PLEdit		re[] = {{slice("b"), {false, {NULL, 0}}}, {slice("z"), {true, {NULL, 0}}}};
	DiffResult	diff = {0};
	unsigned	calls = 0;

	CHECK(store != NULL);
	OK(pl_build(store, entries, 3, &base));
	left = pl_branch(&base);
	right = pl_branch(&base);
	OK(pl_apply(&left, le, 2, &left));
	OK(pl_apply(&right, re, 2, &right));
	OK(pl_diff(&base, &left, count_diff, &diff));
	CHECK(diff.inserts == 1 && diff.updates == 1 && diff.removes == 0);
	OK(pl_merge(&base, &left, &right, NULL, NULL, &merged));
	OK(pl_apply(&left, re, 2, &expected));
	CHECK(pl_hash_equal(merged.root, expected.root));
	OK(pl_merge(&base, &right, &left, NULL, NULL, &expected));
	CHECK(pl_hash_equal(merged.root, expected.root));
	OK(pl_merge(&base, &base, &left, NULL, NULL, &expected));
	CHECK(pl_hash_equal(expected.root, left.root));
	re[0] = (PLEdit)
	{
		slice("a"),
		{
			false,
			{
				NULL, 0
			}
		}
	};
	OK(pl_apply(&base, re, 2, &right));
	unchanged = merged;
	CHECK(pl_merge(&base, &left, &right, NULL, NULL, &merged) == PL_CONFLICT);
	CHECK(pl_hash_equal(merged.root, unchanged.root));
	OK(pl_merge(&base, &left, &right, choose_right, &calls, &merged));
	CHECK(calls == 1);
	OK(pl_apply(&left, re, 2, &expected));
	CHECK(pl_hash_equal(merged.root, expected.root));
	OK(pl_validate(&merged));
	pl_store_free(store);
}

static uint32_t
random_value(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state;
}

static PLStatus
scan_count(void *arg, PLSlice key, PLSlice value)
{
	size_t	   *count = arg;

	(void) key;
	(void) value;
	(*count)++;
	return PL_OK;
}

static PLStatus
stop_scan(void *arg, PLSlice key, PLSlice value)
{
	(void) arg;
	(void) key;
	(void) value;
	return PL_STOP;
}

static PLStatus
stop_diff(void *arg, PLSlice key, PLValue before, PLValue after)
{
	(void) arg;
	(void) key;
	(void) before;
	(void) after;
	return PL_STOP;
}

static void
test_random_history(void)
{
	enum
	{
	SLOTS = 400, STEPS = 1200};
	PLStore    *store = test_store_create();
	PLTree		tree,
				fresh,
				empty;
	int32_t		expected[SLOTS];
	uint8_t		keys[SLOTS][8],
				values[SLOTS][4];
	PLEntry		entries[SLOTS];
	uint32_t	rng = 0x1ab3def;
	unsigned	step,
				i;

	OK(pl_build(store, NULL, 0, &tree));
	empty = tree;
	for (i = 0; i < SLOTS; i++)
	{
		expected[i] = -1;
		pl_key_int64(i, keys[i]);
	}
	for (step = 0; step < STEPS; step++)
	{
		unsigned	which = random_value(&rng) % SLOTS;
		int32_t		value = random_value(&rng) % 4 ? (int32_t) (random_value(&rng) % 100000) : -1;
		uint8_t		encoded[4];
		PLEdit		edit;
		PLTree		old = tree;
		PLValue		previous,
					current;
		size_t		count = 0;
		PLStore    *independent = test_store_create();

		CHECK(independent != NULL);
		pl_write32(encoded, (uint32_t) value);
		edit = (PLEdit)
		{
			{
				keys[which], 8
			},
			{
				value >= 0,
				{
					encoded, 4
				}
			}
		};
		OK(pl_get(&old, edit.key, &previous));
		OK(pl_apply(&tree, &edit, 1, &tree));
		expected[which] = value;
		for (i = 0; i < SLOTS; i++)
			if (expected[i] >= 0)
			{
				pl_write32(values[i], (uint32_t) expected[i]);
				entries[count++] = (PLEntry)
				{
					{
						keys[i], 8
					},
					{
						values[i], 4
					}
				};
			}
		OK(pl_build(independent, entries, count, &fresh));
		CHECK(pl_hash_equal(tree.root, fresh.root));
		CHECK(pl_count(&tree) == count);
		OK(pl_validate(&tree));
		OK(pl_get(&tree, edit.key, &current));
		CHECK(current.present == (value >= 0));
		if (current.present)
			CHECK(pl_read32(current.bytes.data) == (uint32_t) value);
		OK(pl_get(&old, edit.key, &current));
		CHECK(current.present == previous.present);
		if (current.present)
			CHECK(pl_compare(current.bytes, previous.bytes) == 0);
		if (step % 37 == 0)
		{
			DiffResult	result = {0};

			OK(pl_diff(&tree, &fresh, count_diff, &result));
			CHECK(!result.inserts && !result.removes && !result.updates);
		}
		pl_store_free(independent);
	}
	/* Delete all keys in batches and compare final empty root. */
	for (i = 0; i < SLOTS; i++)
	{
		PLEdit		edit = {{keys[i], 8}, {false, {NULL, 0}}};

		OK(pl_apply(&tree, &edit, 1, &tree));
	}
	CHECK(pl_hash_equal(tree.root, empty.root));
	pl_store_free(store);
}

/* Retaining the current root frees the nodes earlier edits left behind. */
static void
test_retain(void)
{
	PLStore    *store = pl_store_create();
	PLTree		tree,
				rebuilt;
	PLHash		root;
	uint8_t		keys[2000][8];
	uint8_t		value[16];
	PLEntry    *entries = calloc(2000, sizeof(PLEntry));
	size_t		before,
				freed;

	CHECK(store && entries);
	CHECK(pl_build(store, NULL, 0, &tree) == PL_OK);
	memset(value, 7, sizeof(value));
	for (int i = 0; i < 2000; i++)
	{
		PLEdit		edit;

		pl_key_int64(i, keys[i]);
		edit.key = (PLSlice) {keys[i], 8};
		edit.value.present = true;
		edit.value.bytes = (PLSlice) {value, sizeof(value)};
		CHECK(pl_apply(&tree, &edit, 1, &tree) == PL_OK);
		entries[i].key = edit.key;
		entries[i].value = edit.value.bytes;
	}
	before = pl_store_stats(store).chunks;
	root = tree.root;
	freed = pl_store_retain(store, &root, 1);
	CHECK(freed > 0 && pl_store_stats(store).chunks == before - freed);
	CHECK(pl_validate(&tree) == PL_OK && pl_count(&tree) == 2000);
	CHECK(pl_build(store, entries, 2000, &rebuilt) == PL_OK && pl_hash_equal(rebuilt.root, tree.root));
	CHECK(pl_store_retain(store, NULL, 0) == before - freed);
	CHECK(pl_store_stats(store).chunks == 0 && pl_store_stats(store).bytes == 0);
	printf("retain: freed %zu of %zu nodes\n", freed, before);
	free(entries);
	pl_store_free(store);
}

static void
test_conflict_matrix(void)
{
	PLStore    *store = test_store_create();
	unsigned	b,
				l,
				r;
	PLSlice		key = slice("key");
	PLValue		variants[] = {{false, {NULL, 0}}, {true, {NULL, 0}},
	{true, {(const uint8_t *) "A", 1}},
	{true, {(const uint8_t *) "B", 1}}};

	for (b = 0; b < 4; b++)
		for (l = 0; l < 4; l++)
			for (r = 0; r < 4; r++)
			{
				PLTree		trees[3],
							merged,
							original;
				unsigned	ids[] = {b, l, r}, i, selected;
				bool		conflict = l != r && l != b && r != b;
				PLValue		value;
				unsigned	calls = 0;

				for (i = 0; i < 3; i++)
				{
					PLEntry		entry = {key, variants[ids[i]].bytes};

					OK(pl_build(store, &entry, variants[ids[i]].present ? 1 : 0, &trees[i]));
				}
				merged = original = trees[0];
				CHECK(pl_merge(&trees[0], &trees[1], &trees[2], NULL, NULL, &merged) ==
					  (conflict ? PL_CONFLICT : PL_OK));
				if (conflict)
					CHECK(pl_hash_equal(merged.root, original.root));
				OK(pl_merge(&trees[0], &trees[1], &trees[2], choose_right, &calls, &merged));
				CHECK(calls == (conflict ? 1U : 0U));
				selected = l == r ? l : l == b ? r : r == b ? l : r;
				OK(pl_get(&merged, key, &value));
				CHECK(value.present == variants[selected].present);
				if (value.present)
					CHECK(pl_compare(value.bytes, variants[selected].bytes) == 0);
			}
	pl_store_free(store);
}

static void
test_ranges_and_shifted_boundaries(void)
{
	PLStore    *store = test_store_create();
	PLTree		before,
				after,
				empty,
				merged;
	PLEntry		entries[2000];
	PLEdit		edits[2000];
	uint8_t		keys[2000][8];
	size_t		i,
				count = 0;
	DiffResult	result = {0};

	for (i = 0; i < 2000; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i] = (PLEntry)
		{
			{
				keys[i], 8
			}, slice("longer tuple to change chunk boundaries")
		};
		edits[i] = (PLEdit)
		{
			{
				keys[i], 8
			},
			{
				i % 3 != 0, slice("x")
			}
		};
	}
	OK(pl_build(store, entries, 2000, &before));
	OK(pl_apply(&before, edits, 2000, &after));
	OK(pl_diff(&before, &after, count_diff, &result));
	CHECK(result.removes == 667 && result.updates == 1333 && result.inserts == 0);
	OK(pl_scan(&before, &entries[100].key, &entries[200].key, scan_count, &count));
	CHECK(count == 100);
	CHECK(pl_scan(&before, NULL, NULL, stop_scan, NULL) == PL_STOP);
	CHECK(pl_diff(&before, &after, stop_diff, NULL) == PL_STOP);
	OK(pl_build(store, NULL, 0, &empty));
	result = (DiffResult)
	{
		0
	};
	OK(pl_diff(&empty, &before, count_diff, &result));
	CHECK(result.inserts == 2000);
	result = (DiffResult)
	{
		0
	};
	OK(pl_diff(&before, &empty, count_diff, &result));
	CHECK(result.removes == 2000);
	CHECK(pl_merge(&before, &after, &empty, NULL, NULL, &merged) == PL_CONFLICT);
	{
		unsigned	calls = 0;

		OK(pl_merge(&before, &after, &empty, choose_right, &calls, &merged));
		CHECK(calls == 1333 && pl_hash_equal(merged.root, empty.root));
	}
	count = 0;
	OK(pl_scan(&before, &entries[200].key, &entries[100].key, scan_count, &count));
	CHECK(count == 0);
	pl_store_free(store);
}


/* A store that loads missing nodes from another store, counting each load. */
typedef struct FetchSource
{
	PLStore    *source;
	unsigned	fetches;
} FetchSource;

static PLStatus
fetch_from_source(void *arg, PLHash hash, PLSlice *bytes)
{
	FetchSource *fetch = arg;

	fetch->fetches++;
	return pl_store_export(fetch->source, hash, bytes);
}

/* A lender of another store's nodes, in place, counting loans still out. */
typedef struct LendSource
{
	PLStore    *source;
	bool		copy;			/* lend transient bytes instead */
	uintptr_t	next_token;
	size_t		outstanding;
	size_t		lent;
} LendSource;

static PLStatus
lend_from_source(void *arg, PLHash hash, PLLoan *loan)
{
	LendSource *lender = arg;
	PLStatus	status = pl_store_export(lender->source, hash, &loan->bytes);

	if (status != PL_OK)
		return status;
	CHECK(pl_node_check(loan->bytes));
	loan->token = lender->copy ? 0 : ++lender->next_token;
	if (loan->token)
	{
		lender->outstanding++;
		lender->lent++;
	}
	return PL_OK;
}

static void
return_to_source(void *arg, uintptr_t token)
{
	LendSource *lender = arg;

	CHECK(token != 0 && token <= lender->next_token && lender->outstanding > 0);
	lender->outstanding--;
}

/*
 * Batched edits on deep trees: every result must equal a fresh build of the
 * same entries, and a small edit on a fetch-backed store must load only a
 * small part of the tree.
 */
static void
test_apply_batches(void)
{
	enum
	{
	SLOTS = 20000, ROUNDS = 60};
	PLStore    *store = pl_store_create();
	uint8_t		(*keys)[8] = calloc(SLOTS, 8);
	uint8_t		(*values)[64] = calloc(SLOTS, 64);
	int32_t    *expected = calloc(SLOTS, sizeof(int32_t));
	PLEntry    *entries = calloc(SLOTS, sizeof(PLEntry));
	PLEdit	   *edits = calloc(SLOTS, sizeof(PLEdit));
	uint8_t		(*edit_values)[64] = calloc(SLOTS, 64);
	uint32_t	rng = 0x5eed;
	PLTree		tree;
	unsigned	round;
	size_t		i,
				count;

	CHECK(store && keys && values && expected && entries && edits && edit_values);
	for (i = 0; i < SLOTS; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		expected[i] = i % 3 ? (int32_t) i : -1;
	}

	/* Values of 4 to 35 bytes exercise both count and byte boundaries. */
#define VALUE_LEN(v) (4 + ((uint32_t) (v) * 2654435761u >> 27))
	count = 0;
	for (i = 0; i < SLOTS; i++)
		if (expected[i] >= 0)
		{
			memset(values[i], (int) expected[i], 64);
			pl_write32(values[i], (uint32_t) expected[i]);
			entries[count++] = (PLEntry)
			{
				{
					keys[i], 8
				}, {
					values[i], VALUE_LEN(expected[i])
				}
			};
		}
	OK(pl_build(store, entries, count, &tree));

	for (round = 0; round < ROUNDS; round++)
	{
		unsigned	shape = round % 6;
		size_t		nedits = 0,
					first = random_value(&rng) % SLOTS,
					span = 1 + random_value(&rng) % (shape == 3 ? 2000 : 200);
		PLStore    *independent = pl_store_create();
		PLTree		fresh;

		CHECK(independent != NULL);
		for (i = 0; i < SLOTS; i++)
		{
			bool		touched;
			int32_t		value;

			switch (shape)
			{
				case 0:			/* scattered */
					touched = random_value(&rng) % 500 == 0;
					break;
				case 1:			/* clustered */
					touched = i >= first && i < first + span;
					break;
				case 2:			/* append past the end, prepend at the start */
					touched = i >= SLOTS - span || i < span / 4;
					break;
				case 3:			/* delete a range */
					touched = i >= first && i < first + span;
					break;
				case 4:			/* one edit */
					touched = i == first;
					break;
				default:		/* dense */
					touched = random_value(&rng) % 7 == 0;
					break;
			}
			if (!touched)
				continue;
			value = shape == 3 || random_value(&rng) % 5 == 0 ? -1 : (int32_t) (random_value(&rng) % 1000000);
			expected[i] = value;
			memset(edit_values[nedits], (int) value, 64);
			pl_write32(edit_values[nedits], (uint32_t) value);
			edits[nedits] = (PLEdit)
			{
				{
					keys[i], 8
				}, {
					value >= 0, {
						edit_values[nedits], VALUE_LEN(value)
					}
				}
			};
			nedits++;
		}
		OK(pl_apply(&tree, edits, nedits, &tree));
		count = 0;
		for (i = 0; i < SLOTS; i++)
			if (expected[i] >= 0)
			{
				memset(values[i], (int) expected[i], 64);
				pl_write32(values[i], (uint32_t) expected[i]);
				entries[count++] = (PLEntry)
				{
					{
						keys[i], 8
					}, {
						values[i], VALUE_LEN(expected[i])
					}
				};
			}
		OK(pl_build(independent, entries, count, &fresh));
		CHECK(pl_hash_equal(tree.root, fresh.root));
		CHECK(pl_count(&tree) == count);
		pl_store_free(independent);
	}
	OK(pl_validate(&tree));

	/* A one-key edit through a fetch-backed store loads only its path's region. */
	{
		FetchSource fetch = {store, 0};
		PLStore    *backed = pl_store_create();
		PLTree		remote = {backed, tree.root};
		uint8_t		changed[4] = {1, 2, 3, 4};
		PLEdit		edit = {{keys[SLOTS / 2], 8}, {true, {changed, 4}}};
		PLStats		stats;

		CHECK(backed != NULL);
		pl_store_set_fetch(backed, fetch_from_source, &fetch);
		OK(pl_apply(&remote, &edit, 1, &remote));
		stats = pl_store_stats(store);
		CHECK(fetch.fetches > 0 && fetch.fetches < 12);
		CHECK(fetch.fetches * 20 < stats.chunks);
		printf("fetch-backed edit: %u fetches of %zu chunks\n", fetch.fetches, stats.chunks);

		/* Dropping unbacked nodes removes exactly the edit's new nodes and keeps the fetched ones. */
		stats = pl_store_stats(backed);
		CHECK(stats.chunks > stats.fetched);
		CHECK(pl_store_drop_unbacked(backed) == stats.chunks - stats.fetched);
		CHECK(pl_store_stats(backed).chunks == stats.fetched);
		CHECK(pl_store_drop_unbacked(backed) == 0);
		CHECK(pl_store_stats(backed).chunks == stats.fetched);

		/* Unbacked nodes that retain frees are forgotten, not freed again. */
		{
			PLTree		base_remote = {backed, tree.root},
						again;
			uint8_t		other[4] = {5, 6, 7, 8};
			PLEdit		edit2 = {{keys[SLOTS / 3], 8}, {true, {other, 4}}};
			PLStats		before = pl_store_stats(backed),
						after;
			size_t		built;

			OK(pl_apply(&base_remote, &edit2, 1, &again));
			after = pl_store_stats(backed);
			built = (after.chunks - before.chunks) - (after.fetched - before.fetched);
			CHECK(built > 0);
			CHECK(pl_store_retain(backed, &tree.root, 1) == built);
			CHECK(pl_store_drop_unbacked(backed) == 0);
			CHECK(pl_store_stats(backed).chunks == after.fetched);
		}

		/* A pinned node outlives the drop until something writes it. */
		{
			PLTree		empty;
			size_t		chunks = pl_store_stats(backed).chunks;

			OK(pl_build(backed, NULL, 0, &empty));
			CHECK(pl_store_stats(backed).chunks == chunks + 1);
			pl_node_pin(backed, pl_find_local(backed, empty.root));
			CHECK(pl_store_drop_unbacked(backed) == 0);
			CHECK(pl_find_local(backed, empty.root) != NULL);
			CHECK(!pl_find_local(backed, empty.root)->backed);
			pl_node_set_backed(pl_find_local(backed, empty.root));
		}

		/*
		 * Pins count, and a pinned node survives collection, with what it
		 * reaches, though no root names it.
		 */
		{
			PLTree		base_remote = {backed, tree.root},
						edited;
			uint8_t		other[4] = {7, 7, 7, 7};
			PLEdit		edit3 = {{keys[SLOTS / 5], 8}, {true, {other, 4}}};
			PLNode	   *root;

			OK(pl_apply(&base_remote, &edit3, 1, &edited));
			root = pl_find_local(backed, edited.root);
			CHECK(root != NULL && !root->backed && root->level > 0);
			pl_node_pin(backed, root);
			pl_node_pin(backed, root);
			pl_store_retain(backed, &tree.root, 1);
			CHECK(pl_find_local(backed, edited.root) == root);
			{
				PLValue		got;

				OK(pl_get(&edited, edit3.key, &got));
				CHECK(got.present && got.bytes.len == 4 && memcmp(got.bytes.data, other, 4) == 0);
			}
			pl_node_unpin(backed, root);
			pl_store_retain(backed, &tree.root, 1);
			CHECK(pl_find_local(backed, edited.root) == root);
			pl_node_unpin(backed, root);
			pl_store_retain(backed, &tree.root, 1);
			CHECK(pl_find_local(backed, edited.root) == NULL);
		}
		pl_store_free(backed);
	}

	/*
	 * A lending store reads lent nodes in place, and returns each loan once:
	 * when collection frees the node, when the store returns its loans, or
	 * when it is freed.
	 */
	{
		LendSource	lender = {store, false, 0, 0, 0};
		PLStore    *lending = pl_store_create();
		PLTree		remote = {lending, tree.root},
					edited,
					expected_tree;
		uint8_t		changed[4] = {4, 3, 2, 1};
		PLEdit		edit = {{keys[SLOTS / 7], 8}, {true, {changed, 4}}};
		PLValue		got;
		PLNode	   *root;
		PLStats		stats;

		CHECK(lending != NULL);
		pl_store_set_lender(lending, lend_from_source, return_to_source, &lender);
		OK(pl_apply(&tree, &edit, 1, &expected_tree));
		OK(pl_apply(&remote, &edit, 1, &edited));
		CHECK(pl_hash_equal(edited.root, expected_tree.root));
		stats = pl_store_stats(lending);
		CHECK(lender.outstanding > 0 && stats.borrowed == lender.outstanding);
		CHECK(stats.fetched == lender.lent);
		root = pl_find_local(lending, tree.root);
		CHECK(root != NULL && root->backed && root->loan != 0);
		CHECK(root->data == pl_find_local(store, tree.root)->data);
		CHECK(root->level == pl_find_local(store, tree.root)->level);

		/* Collection returns the loans of the nodes it frees. */
		CHECK(pl_store_retain(lending, &edited.root, 1) > 0);
		CHECK(pl_store_stats(lending).borrowed == lender.outstanding);
		CHECK(pl_find_local(lending, tree.root) == NULL);
		OK(pl_get(&edited, edit.key, &got));
		CHECK(got.present && got.bytes.len == 4 && memcmp(got.bytes.data, changed, 4) == 0);

		/* Returning the loans keeps the store's own nodes; finds borrow again. */
		{
			size_t		out = lender.outstanding;

			CHECK(pl_store_return_loans(lending) == out);
		}
		CHECK(lender.outstanding == 0 && pl_store_stats(lending).borrowed == 0);
		CHECK(pl_find_local(lending, edited.root) != NULL);
		OK(pl_get(&remote, edit.key, &got));
		CHECK(lender.outstanding > 0);
		pl_store_free(lending);
		CHECK(lender.outstanding == 0);

		/* Transient loans are copied and checked like fetched bytes. */
		lending = pl_store_create();
		CHECK(lending != NULL);
		lender.copy = true;
		pl_store_set_lender(lending, lend_from_source, return_to_source, &lender);
		remote.store = lending;
		OK(pl_get(&remote, edit.key, &got));
		CHECK(pl_store_stats(lending).fetched > 0 && pl_store_stats(lending).borrowed == 0);
		CHECK(pl_find_local(lending, tree.root)->data != pl_find_local(store, tree.root)->data);
		CHECK(pl_store_return_loans(lending) == 0);
		pl_store_free(lending);
		CHECK(lender.outstanding == 0);
	}

	/* Diff and merge of small changes on a large tree also stay local. */
	{
		FetchSource fetch = {store, 0};
		PLStore    *backed = pl_store_create();
		uint8_t		one[4] = {9, 9, 9, 9},
					two[4] = {8, 8, 8, 8};
		PLEdit		left_edit = {{keys[100], 8}, {true, {one, 4}}};
		PLEdit		right_edit = {{keys[SLOTS - 100], 8}, {true, {two, 4}}};
		PLTree		left,
					right,
					merged,
					expected_tree;
		DiffResult	result = {0};

		OK(pl_apply(&tree, &left_edit, 1, &left));
		OK(pl_apply(&tree, &right_edit, 1, &right));
		OK(pl_apply(&left, &right_edit, 1, &expected_tree));
		CHECK(backed != NULL);
		pl_store_set_fetch(backed, fetch_from_source, &fetch);
		{
			PLTree		base_remote = {backed, tree.root},
						left_remote = {backed, left.root},
						right_remote = {backed, right.root};

			OK(pl_diff(&base_remote, &left_remote, count_diff, &result));
			CHECK(result.updates + result.inserts == 1 && !result.removes);
			CHECK(fetch.fetches < 12);
			OK(pl_merge(&base_remote, &left_remote, &right_remote, NULL, NULL, &merged));
			CHECK(pl_hash_equal(merged.root, expected_tree.root));
			CHECK(fetch.fetches < 30);
			printf("fetch-backed diff and merge: %u fetches\n", fetch.fetches);
		}
		pl_store_free(backed);
	}
#undef VALUE_LEN
	free(keys);
	free(values);
	free(expected);
	free(entries);
	free(edits);
	free(edit_values);
	pl_store_free(store);
}

static void
test_import_and_malformed(void)
{
	PLStore    *source = test_store_create(),
			   *target = test_store_create();
	PLTree		tree,
				opened,
				untouched;
	PLEntry		entries[1000];
	uint8_t		keys[1000][8];
	size_t		i;

	for (i = 0; i < 1000; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i] = (PLEntry)
		{
			{
				keys[i], 8
			}, slice("payload")
		};
	}
	OK(pl_build(source, entries, 1000, &tree));
	untouched = tree;
	CHECK(pl_open(target, tree.root, &opened) == PL_CORRUPT);
	for (i = 0; i < source->capacity; i++)
	{
		PLNode	   *node;

		for (node = source->buckets[i]; node; node = node->next)
		{
			PLSlice		data;
			uint8_t    *bad;

			OK(pl_store_export(source, node->hash, &data));
			OK(pl_store_import(target, node->hash, data));
			bad = malloc(data.len);
			CHECK(bad != NULL);
			memcpy(bad, data.data, data.len);
			bad[data.len - 1] ^= 1;
			CHECK(pl_store_import(target, node->hash, bytes(bad, data.len)) == PL_CORRUPT);
			free(bad);
		}
	}
	OK(pl_open(target, tree.root, &opened));
	CHECK(pl_count(&opened) == 1000);
	{
		DiffResult	result = {0};

		OK(pl_diff(&tree, &opened, count_diff, &result));
		CHECK(!result.inserts && !result.removes && !result.updates);
	}
	/* Digest-valid invalid encodings must fail import too. */
	{
		PLSlice		data;
		PLHash		hash;
		uint8_t    *bad;
		size_t		width;

		OK(pl_store_export(source, tree.root, &data));
		bad = malloc(data.len);
		CHECK(bad != NULL);
		memcpy(bad, data.data, data.len);
		width = pl_read32(bad + 16) <= PL_NARROW_LIMIT && pl_read32(bad + 20) <= PL_NARROW_LIMIT ? 2 : 4;
		bad[PL_HEADER + width - 1] = 1; /* first key offset must be zero */
		CHECK(pl_node_address(bytes(bad, data.len), &hash));
		CHECK(pl_store_import(target, hash, bytes(bad, data.len)) == PL_CORRUPT);
		CHECK(pl_node_check(data) && !pl_node_check(bytes(bad, data.len)));
		CHECK(!pl_node_check(bytes(data.data, PL_HEADER - 1)));
		memcpy(bad, data.data, data.len);
		/* Valid local encoding, but nonexistent child address. */
		bad[PL_HEADER + 2 * width * (pl_read32(bad + 12) + 1) + pl_read32(bad + 16)] ^= 128;
		CHECK(pl_node_address(bytes(bad, data.len), &hash));
		OK(pl_store_import(target, hash, bytes(bad, data.len)));
		CHECK(pl_open(target, hash, &untouched) == PL_CORRUPT);
		CHECK(pl_hash_equal(untouched.root, tree.root));
		free(bad);
	}
	pl_store_free(source);
	pl_store_free(target);
}

static void
test_large_and_binary_tuples(void)
{
	PLStore    *store = test_store_create();
	PLTree		tree,
				changed,
				fresh;
	uint8_t    *large = malloc(100000);
	const uint8_t binary[] = {0, 255, 0, 128};
	PLEntry		entries[] = {{{NULL, 0}, {NULL, 0}}, {{binary, 4}, {NULL, 0}}};
	PLEdit		edit;
	PLValue		got;

	CHECK(large != NULL);
	memset(large, 97, 100000);
	entries[1].value = (PLSlice)
	{
		large, 100000
	};
	OK(pl_build(store, entries, 2, &tree));
	OK(pl_get(&tree, entries[0].key, &got));
	CHECK(got.present && got.bytes.len == 0);
	OK(pl_get(&tree, entries[1].key, &got));
	CHECK(got.present && got.bytes.len == 100000);
	CHECK(memcmp(got.bytes.data, large, 100000) == 0);
	{
		/* Values past 64 kB keep four-byte directory offsets: tuple size is not limited. */
		PLNode	   *leaf = pl_find(store, tree.root);

		CHECK(leaf && leaf->level == 0);
		CHECK(leaf->offset_width == 4);
		CHECK(leaf->size == PL_HEADER + 2 * 4 * (leaf->count + 1) + leaf->key_bytes + leaf->value_bytes);
	}
	edit = (PLEdit)
	{
		entries[1].key,
		{
			true, slice("short")
		}
	};
	OK(pl_apply(&tree, &edit, 1, &changed));
	entries[1].value = slice("short");
	OK(pl_build(store, entries, 2, &fresh));
	CHECK(pl_hash_equal(changed.root, fresh.root));
	entries[0].key = (PLSlice)
	{
		binary, sizeof(binary)
	};
	CHECK(pl_build(store, entries, 2, &changed) == PL_INVALID);
	CHECK(pl_hash_equal(changed.root, fresh.root));
	entries[0].key = (PLSlice)
	{
		NULL, 1
	};
	CHECK(pl_build(store, entries, 2, &changed) == PL_INVALID);
	free(large);
	pl_store_free(store);
}

static PLStatus
resolve_scratch(void *arg, PLSlice key, PLValue base, PLValue left, PLValue right, PLValue *out)
{
	uint8_t    *scratch = arg;

	(void) base;
	(void) left;
	(void) right;
	scratch[0] = key.data[0];
	*out = (PLValue)
	{
		true,
		{
			scratch, 1
		}
	};
	return PL_OK;
}

static void
test_format_and_resolver_lifetime(void)
{
	PLStore    *store = test_store_create();
	PLTree		empty,
				left,
				right,
				merged;
	PLSlice		serialized;
	char		hex[65];
	unsigned	i;
	PLEntry		a[] = {{slice("a"), slice("left")}, {slice("b"), slice("left")}};
	PLEntry		b[] = {{slice("a"), slice("right")}, {slice("b"), slice("right")}};
	uint8_t		scratch[1];
	PLValue		value;

	OK(pl_build(store, NULL, 0, &empty));
	OK(pl_store_export(store, empty.root, &serialized));
	/* The empty tree's address pins the format: its SHA-256 truncated to the hash size. */
	CHECK(serialized.len == 28);
	for (i = 0; i < PL_HASH_SIZE; i++)
		snprintf(hex + 2 * i, 3, "%02x", empty.root.bytes[i]);
	CHECK(strcmp(hex, "c611f574ef24eb5785f74f9277220117b134de73") == 0);
	OK(pl_build(store, a, 2, &left));
	OK(pl_build(store, b, 2, &right));
	OK(pl_merge(&empty, &left, &right, resolve_scratch, scratch, &merged));
	scratch[0] = 'X';
	OK(pl_get(&merged, slice("a"), &value));
	CHECK(pl_compare(value.bytes, slice("a")) == 0);
	OK(pl_get(&merged, slice("b"), &value));
	CHECK(pl_compare(value.bytes, slice("b")) == 0);
	pl_store_free(store);
}

static void
test_hash_text(void)
{
	PLHash		hash = {{0}}, parsed;
	char text[	PL_HASH_TEXT_SIZE + 1];
	int			i;

	pl_hash_format(hash, text);
	CHECK(strcmp(text, "00000000000000000000000000000000") == 0);
	for (i = 0; i < PL_HASH_SIZE; i++)
		hash.bytes[i] = (uint8_t) i;
	pl_hash_format(hash, text);
	CHECK(strcmp(text, "000g40o40k30e209185go38e1s8124gj") == 0);
	CHECK(pl_hash_parse(text, &parsed) && pl_hash_equal(hash, parsed));
	memset(hash.bytes, 255, PL_HASH_SIZE);
	pl_hash_format(hash, text);
	CHECK(strcmp(text, "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv") == 0);
	CHECK(pl_hash_parse(text, &parsed) && pl_hash_equal(hash, parsed));
	CHECK(!pl_hash_parse("0000000000000000000000000000000", &parsed));
	CHECK(!pl_hash_parse("000000000000000000000000000000000", &parsed));
	CHECK(!pl_hash_parse("w0000000000000000000000000000000", &parsed));
	CHECK(!pl_hash_parse("V0000000000000000000000000000000", &parsed));
	for (i = 0; i < PL_HASH_SIZE * 8; i++)
	{
		memset(hash.bytes, 0, PL_HASH_SIZE);
		hash.bytes[i / 8] = 1 << (i % 8);
		pl_hash_format(hash, text);
		CHECK(pl_hash_parse(text, &parsed) && pl_hash_equal(hash, parsed));
	}
}

int
main(int argc, char **argv)
{
	file_mode = argc == 2 && strcmp(argv[1], "--file") == 0;
	test_hash_text();
	test_tuples();
	test_tuple_view();
	test_tree();
	test_node_sizes();
	test_child_references();
	test_versions();
	test_random_history();
	test_apply_batches();
	test_retain();
	test_conflict_matrix();
	test_ranges_and_shifted_boundaries();
	test_import_and_malformed();
	test_large_and_binary_tuples();
	test_format_and_resolver_lifetime();
	printf("passed %u checks\n", checks);
	return 0;
}
