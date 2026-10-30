/* Ordered cursors and deterministic streaming construction. */
#include "internal.h"
#include <limits.h>

typedef struct PLBuilder
{
	PLStore    *store;
	PLEntry		pending[PL_MAX_ENTRIES];
	uint32_t	count;
	size_t		bytes;
	PLNode	  **nodes;
	size_t		used;
	size_t		capacity;
} PLBuilder;

static bool
valid_slice(PLSlice slice)
{
	return slice.len <= PL_MAX_ITEM && (slice.len == 0 || slice.data != NULL);
}

static uint32_t
lower_bound(const PLNode *node, PLSlice key)
{
	uint32_t	lo = 0,
				hi = node->count;

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2;

		if (pl_compare(pl_node_key(node, mid), key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static PLStatus
cursor_descend(PLCursor *cursor, PLNode *node, const PLSlice *lower)
{
	while (true)
	{
		uint32_t	index = lower ? lower_bound(node, *lower) : 0;

		if (cursor->depth >= PL_MAX_DEPTH)
			return PL_CORRUPT;
		cursor->path[cursor->depth] = node;
		cursor->index[cursor->depth++] = index;
		if (index == node->count)
		{
			cursor->valid = false;
			return PL_OK;
		}
		if (!node->level)
		{
			cursor->valid = true;
			return PL_OK;
		}
		else
		{
			PLHash		hash;
			PLNode	   *child;

			hash = pl_child_hash(node, index);
			child = pl_find(cursor->store, hash);
			if (!child || child->level + 1 != node->level)
				return PL_CORRUPT;
			node = child;
		}
	}
}

PLStatus
pl_cursor_init(PLCursor *cursor, const PLTree *tree, const PLSlice *lower)
{
	PLNode	   *root;

	memset(cursor, 0, sizeof(*cursor));
	if (!tree || !tree->store || (lower && !valid_slice(*lower)))
		return PL_INVALID;
	root = pl_find(tree->store, tree->root);
	if (!root)
		return PL_CORRUPT;
	cursor->store = tree->store;
	return cursor_descend(cursor, root, lower);
}

PLEntry
pl_cursor_entry(const PLCursor *cursor)
{
	uint32_t	depth = cursor->depth - 1;
	PLEntry		entry = {pl_node_key(cursor->path[depth], cursor->index[depth]),
	pl_node_value(cursor->path[depth], cursor->index[depth])};

	return entry;
}

PLStatus
pl_cursor_skip_leaf(PLCursor *cursor)
{
	if (!cursor->valid)
		return PL_OK;
	cursor->depth--;
	while (cursor->depth)
	{
		uint32_t	depth = cursor->depth - 1;
		PLNode	   *parent = cursor->path[depth];

		if (++cursor->index[depth] < parent->count)
		{
			PLHash		hash;
			PLNode	   *child;

			hash = pl_child_hash(parent, cursor->index[depth]);
			child = pl_find(cursor->store, hash);
			if (!child || child->level + 1 != parent->level)
				return PL_CORRUPT;
			return cursor_descend(cursor, child, NULL);
		}
		cursor->depth--;
	}
	cursor->valid = false;
	return PL_OK;
}

/* Whether the cursor is at the first entry of the subtree rooted at path[depth]. */
bool
pl_cursor_at_subtree_start(const PLCursor *cursor, uint32_t depth)
{
	for (uint32_t k = depth; k < cursor->depth; k++)
		if (cursor->index[k] != 0)
			return false;
	return true;
}

/* Move past the whole subtree rooted at path[depth]. */
PLStatus
pl_cursor_skip_subtree(PLCursor *cursor, uint32_t depth)
{
	if (!cursor->valid)
		return PL_OK;
	if (depth == 0)
	{
		cursor->valid = false;
		return PL_OK;
	}
	cursor->depth = depth + 1;
	return pl_cursor_skip_leaf(cursor);
}

PLStatus
pl_cursor_next(PLCursor *cursor)
{
	uint32_t	depth;

	if (!cursor->valid)
		return PL_OK;
	depth = cursor->depth - 1;
	if (++cursor->index[depth] < cursor->path[depth]->count)
		return PL_OK;
	return pl_cursor_skip_leaf(cursor);
}

/*
 * Descend from |node| to the last entry of its subtree whose key is below
 * |upper|, or its last entry if |upper| is NULL. An interior key is its
 * child's last key, so the first child whose key reaches |upper| holds the
 * last entries below it, if any do.
 */
static PLStatus
cursor_descend_last(PLCursor *cursor, PLNode *node, const PLSlice *upper)
{
	while (true)
	{
		uint32_t	index = upper ? lower_bound(node, *upper) : node->count;

		if (cursor->depth >= PL_MAX_DEPTH)
			return PL_CORRUPT;
		cursor->path[cursor->depth] = node;
		if (!node->level)
		{
			/* index entries are below |upper|: step back from the first that is not. */
			cursor->index[cursor->depth++] = index;
			cursor->valid = true;
			return pl_cursor_prev(cursor);
		}
		if (index == node->count)
		{
			index = node->count - 1;
			upper = NULL;
		}
		cursor->index[cursor->depth++] = index;
		{
			PLHash		hash = pl_child_hash(node, index);
			PLNode	   *child = pl_find(cursor->store, hash);

			if (!child || child->level + 1 != node->level)
				return PL_CORRUPT;
			node = child;
		}
	}
}

PLStatus
pl_cursor_init_last(PLCursor *cursor, const PLTree *tree, const PLSlice *upper)
{
	PLNode	   *root;

	memset(cursor, 0, sizeof(*cursor));
	if (!tree || !tree->store || (upper && !valid_slice(*upper)))
		return PL_INVALID;
	root = pl_find(tree->store, tree->root);
	if (!root)
		return PL_CORRUPT;
	cursor->store = tree->store;
	if (!root->count)
		return PL_OK;
	return cursor_descend_last(cursor, root, upper);
}

/*
 * Move to the previous entry; past the first, the cursor is invalid. A leaf's
 * index may be its count, just past its last entry, as cursor_descend_last
 * leaves it.
 */
PLStatus
pl_cursor_prev(PLCursor *cursor)
{
	uint32_t	depth;

	if (!cursor->valid)
		return PL_OK;
	depth = cursor->depth - 1;
	if (cursor->index[depth] > 0)
	{
		cursor->index[depth]--;
		return PL_OK;
	}
	cursor->depth--;
	while (cursor->depth)
	{
		uint32_t	parent_depth = cursor->depth - 1;
		PLNode	   *parent = cursor->path[parent_depth];

		if (cursor->index[parent_depth] > 0)
		{
			PLHash		hash;
			PLNode	   *child;

			hash = pl_child_hash(parent, --cursor->index[parent_depth]);
			child = pl_find(cursor->store, hash);
			if (!child || child->level + 1 != parent->level)
				return PL_CORRUPT;
			return cursor_descend_last(cursor, child, NULL);
		}
		cursor->depth--;
	}
	cursor->valid = false;
	return PL_OK;
}

static PLStatus
append_node(PLBuilder *builder, PLNode *node)
{
	if (builder->used == builder->capacity)
	{
		size_t		capacity = builder->capacity ? builder->capacity * 2 : 16;
		PLNode	  **nodes;

		if (capacity < builder->capacity || capacity > SIZE_MAX / sizeof(*nodes))
			return PL_NOMEM;
		nodes = realloc(builder->nodes, capacity * sizeof(*nodes));
		if (!nodes)
			return PL_NOMEM;
		builder->nodes = nodes;
		builder->capacity = capacity;
	}
	builder->nodes[builder->used++] = node;
	return PL_OK;
}

static PLStatus
flush(PLBuilder *builder, uint32_t level)
{
	PLNode	   *node;
	PLStatus	status = pl_save(builder->store, level, builder->pending, builder->count, &node);

	if (status != PL_OK)
		return status;
	status = append_node(builder, node);
	builder->count = 0;
	builder->bytes = 0;
	return status;
}

static PLStatus
add(PLBuilder *builder, PLEntry entry, uint32_t level)
{
	builder->pending[builder->count++] = entry;
	builder->bytes += PL_ENTRY_OVERHEAD + entry.key.len + entry.value.len;
	if (pl_boundary(level, builder->count, builder->bytes, PL_ENTRY_OVERHEAD + entry.key.len + entry.value.len, entry.key))
		return flush(builder, level);
	return PL_OK;
}

static PLStatus
finish(PLBuilder *builder, PLTree *out)
{
	uint32_t	level = 0;
	PLStatus	status = PL_OK;

	if (builder->count || !builder->used)
		status = flush(builder, 0);
	while (status == PL_OK && builder->used > 1)
	{
		PLNode	  **old = builder->nodes;
		size_t		count = builder->used,
					i;
		uint8_t		child_data[PL_MAX_ENTRIES][PL_CHILD_MAX];

		builder->nodes = NULL;
		builder->used = builder->capacity = 0;
		if (++level >= PL_MAX_DEPTH)
		{
			free(old);
			return PL_INVALID;
		}
		for (i = 0; i < count && status == PL_OK; i++)
		{
			PLEntry		entry;
			uint8_t    *child = child_data[builder->count];

			entry.key = pl_node_key(old[i], old[i]->count - 1);
			entry.value.data = child;
			entry.value.len = pl_child_encode(child, old[i]->hash, pl_node_rows(old[i]));
			status = add(builder, entry, level);
		}
		if (status == PL_OK && builder->count)
			status = flush(builder, level);
		free(old);
	}
	if (status == PL_OK)
	{
		out->store = builder->store;
		out->root = builder->nodes[0]->hash;
	}
	return status;
}

PLStatus
pl_build(PLStore *store, const PLEntry *entries, size_t count, PLTree *out)
{
	PLBuilder	builder = {0};
	PLStatus	status = PL_OK;
	size_t		i;

	if (!store || !out || (count && !entries))
		return PL_INVALID;
	for (i = 0; i < count; i++)
		if (!valid_slice(entries[i].key) || !valid_slice(entries[i].value) ||
			(i && pl_compare(entries[i - 1].key, entries[i].key) >= 0))
			return PL_INVALID;
	builder.store = store;
	for (i = 0; i < count && status == PL_OK; i++)
		status = add(&builder, entries[i], 0);
	if (status == PL_OK)
		status = finish(&builder, out);
	free(builder.nodes);
	return status;
}

PLTree
pl_branch(const PLTree *tree)
{
	return *tree;
}

/*
 * Path-local edits.
 *
 * A stream builds every level of a tree at once. Each level holds the pending
 * entries of the node it is building and flushes at the same content-defined
 * boundaries pl_build uses, so the result is identical to rebuilding the whole
 * tree from its entries.
 *
 * Applying edits streams only the region around them. Every node except the
 * last at its level ends at a natural boundary, so an untouched node can be
 * appended by reference, without fetching it, whenever every level at or below
 * its own is empty: streaming its contents would rebuild exactly that node. The
 * last node at a level ends only because the tree ended there, so it is reused
 * by reference only once no edits remain to follow it.
 */
typedef struct PLLevel
{
	PLEntry		pending[PL_MAX_ENTRIES];
	uint8_t		values[PL_MAX_ENTRIES][PL_CHILD_MAX];
	uint32_t	count;
	size_t		bytes;
	uint64_t	entries;		/* entries ever added at this level */
	bool		reused;			/* an existing node was appended at this level */
	PLHash		last;			/* the child most recently added here */
} PLLevel;

typedef struct PLStream
{
	PLStore    *store;
	PLLevel	   *levels;			/* PL_MAX_DEPTH + 1 levels */
} PLStream;

static PLStatus stream_flush(PLStream *stream, uint32_t level);

/* Add a child reference at |level| (one above the child's own level). */
static PLStatus
stream_child(PLStream *stream, uint32_t level, PLSlice key, PLHash hash, uint64_t rows, bool reused)
{
	PLLevel    *l;
	uint8_t    *value;
	size_t		length;

	if (level >= PL_MAX_DEPTH)
		return PL_INVALID;
	l = &stream->levels[level];
	value = l->values[l->count];
	length = pl_child_encode(value, hash, rows);
	l->pending[l->count].key = key;
	l->pending[l->count].value.data = value;
	l->pending[l->count].value.len = length;
	l->count++;
	l->bytes += PL_ENTRY_OVERHEAD + key.len + length;
	l->entries++;
	l->last = hash;
	l->reused |= reused;
	if (pl_boundary(level, l->count, l->bytes, PL_ENTRY_OVERHEAD + key.len + length, key))
		return stream_flush(stream, level);
	return PL_OK;
}

static PLStatus
stream_entry(PLStream *stream, PLEntry entry)
{
	PLLevel    *l = &stream->levels[0];

	l->pending[l->count++] = entry;
	l->bytes += PL_ENTRY_OVERHEAD + entry.key.len + entry.value.len;
	l->entries++;
	if (pl_boundary(0, l->count, l->bytes, PL_ENTRY_OVERHEAD + entry.key.len + entry.value.len, entry.key))
		return stream_flush(stream, 0);
	return PL_OK;
}

static PLStatus
stream_flush(PLStream *stream, uint32_t level)
{
	PLLevel    *l = &stream->levels[level];
	PLNode	   *node;
	PLStatus	status = pl_save(stream->store, level, l->pending, l->count, &node);

	if (status != PL_OK)
		return status;
	l->count = 0;
	l->bytes = 0;
	return stream_child(stream, level + 1, pl_node_key(node, node->count - 1), node->hash,
						pl_node_rows(node), false);
}

/*
 * Flush every level and find the root: the lowest level with exactly one node.
 * A reused node hides its descendants, so a level only counts as having one
 * node if no node from a higher level was reused.
 */
static PLStatus
stream_finish(PLStream *stream, PLTree *out)
{
	PLNode	   *root = NULL;
	uint32_t	level;
	PLStatus	status = PL_OK;

	if (!stream->levels[0].entries && !stream->levels[1].entries)
	{
		bool		empty = true;

		for (level = 2; level < PL_MAX_DEPTH; level++)
			empty &= stream->levels[level].entries == 0;
		if (empty)
		{
			status = pl_save(stream->store, 0, NULL, 0, &root);
			if (status == PL_OK)
			{
				out->store = stream->store;
				out->root = root->hash;
			}
			return status;
		}
	}
	for (level = 0; level + 1 < PL_MAX_DEPTH && status == PL_OK; level++)
	{
		bool		hidden = false;
		uint32_t	above;

		if (stream->levels[level].count)
			status = stream_flush(stream, level);
		if (status != PL_OK)
			break;
		for (above = level + 2; above < PL_MAX_DEPTH; above++)
			hidden |= stream->levels[above].reused;
		if (stream->levels[level + 1].entries == 1 && !hidden)
		{
			root = pl_find(stream->store, stream->levels[level + 1].last);
			break;
		}
	}
	if (status != PL_OK)
		return status;
	if (!root)
		return PL_CORRUPT;

	/* A reused internal node with one child is not a root pl_build would make. */
	while (root->level && root->count == 1)
	{
		PLHash		child;

		child = pl_child_hash(root, 0);
		root = pl_find(stream->store, child);
		if (!root)
			return PL_CORRUPT;
	}
	out->store = stream->store;
	out->root = root->hash;
	return PL_OK;
}

PLStatus
pl_apply(const PLTree *tree, const PLEdit *edits, size_t count, PLTree *out)
{
	PLStream	stream = {0};
	PLNode	   *path[PL_MAX_DEPTH];
	uint32_t	next[PL_MAX_DEPTH];
	bool		right[PL_MAX_DEPTH];
	uint32_t	depth = 0,
				entry = 0;
	PLNode	   *node;
	PLStatus	status = PL_OK;
	size_t		i;
	bool		edge = true;

	if (!tree || !out || (count && !edits))
		return PL_INVALID;
	for (i = 0; i < count; i++)
		if (!valid_slice(edits[i].key) || (edits[i].value.present && !valid_slice(edits[i].value.bytes)) ||
			(i && pl_compare(edits[i - 1].key, edits[i].key) >= 0))
			return PL_INVALID;
	node = pl_find(tree->store, tree->root);
	if (!node)
		return PL_CORRUPT;
	if (!count)
	{
		*out = *tree;
		return PL_OK;
	}
	stream.store = tree->store;
	stream.levels = calloc(PL_MAX_DEPTH + 1, sizeof(PLLevel));
	if (!stream.levels)
		return PL_NOMEM;

	/*
	 * Descend to the leaf holding the first edit, reusing every subtree to
	 * its left. Those subtrees precede the path, so all levels below them are
	 * empty when each is appended.
	 */
	while (status == PL_OK)
	{
		uint32_t	c;

		if (depth >= PL_MAX_DEPTH)
		{
			status = PL_CORRUPT;
			break;
		}
		path[depth] = node;
		right[depth] = edge;
		if (!node->level)
		{
			depth++;
			break;
		}
		c = lower_bound(node, edits[0].key);
		if (c >= node->count)
			c = node->count - 1;
		for (uint32_t j = 0; j < c && status == PL_OK; j++)
		{
			status = stream_child(&stream, node->level, pl_node_key(node, j), pl_child_hash(node, j),
								  pl_child_rows(node, j), true);
		}
		next[depth++] = c + 1;
		edge = edge && c == node->count - 1;
		{
			PLHash		hash;

			hash = pl_child_hash(node, c);
			node = pl_find(tree->store, hash);
		}
		if (!node || node->level + 1 != path[depth - 1]->level)
			status = PL_CORRUPT;
	}

	i = 0;
	while (status == PL_OK)
	{
		PLNode	   *leaf = path[depth - 1];

		/* Merge this leaf's entries with the edits that fall inside it. */
		while (status == PL_OK && entry < leaf->count)
		{
			PLEntry		current = {pl_node_key(leaf, entry), pl_node_value(leaf, entry)};
			int			comparison = i == count ? -1 : pl_compare(current.key, edits[i].key);

			if (comparison < 0)
			{
				status = stream_entry(&stream, current);
				entry++;
			}
			else
			{
				if (edits[i].value.present)
				{
					PLEntry		edited = {edits[i].key, edits[i].value.bytes};

					status = stream_entry(&stream, edited);
				}
				if (comparison == 0)
					entry++;
				i++;
			}
		}
		if (status != PL_OK)
			break;

		/*
		 * Move to the next leaf, appending whole untouched subtrees whenever
		 * the levels at and below them are empty.
		 */
		depth--;
		while (depth)
		{
			PLNode	   *parent = path[depth - 1];
			uint32_t	c,
						level;
			bool		child_right,
						skip;
			PLSlice		last;
			PLHash		hash;

			if (next[depth - 1] >= parent->count)
			{
				depth--;
				continue;
			}
			c = next[depth - 1]++;
			level = parent->level - 1;
			child_right = right[depth - 1] && c == parent->count - 1;
			last = pl_node_key(parent, c);
			hash = pl_child_hash(parent, c);
			skip = (i == count || pl_compare(edits[i].key, last) > 0) && (!child_right || i == count);
			for (uint32_t k = 0; skip && k <= level; k++)
				skip = stream.levels[k].count == 0;
			if (skip)
			{
				stream.store->stats.skipped++;
				status = stream_child(&stream, parent->level, last, hash,
									  pl_child_rows(parent, c), true);
				if (status != PL_OK)
					break;
				continue;
			}
			node = pl_find(stream.store, hash);
			if (!node || node->level != level)
			{
				status = PL_CORRUPT;
				break;
			}
			path[depth] = node;
			right[depth] = child_right;
			next[depth] = 0;
			depth++;
			if (!level)
			{
				entry = 0;
				break;
			}
		}
		if (status != PL_OK || !depth)
			break;
	}

	/* Edits past the old last key extend the tree. */
	for (; status == PL_OK && i < count; i++)
		if (edits[i].value.present)
		{
			PLEntry		edited = {edits[i].key, edits[i].value.bytes};

			status = stream_entry(&stream, edited);
		}
	if (status == PL_OK)
		status = stream_finish(&stream, out);
	free(stream.levels);
	return status;
}

PLStatus
pl_get(const PLTree *tree, PLSlice key, PLValue *out)
{
	PLCursor	cursor;
	PLStatus	status;

	if (!out)
		return PL_INVALID;
	status = pl_cursor_init(&cursor, tree, &key);
	if (status != PL_OK)
		return status;
	if (cursor.valid && pl_compare(pl_cursor_entry(&cursor).key, key) == 0)
	{
		out->present = true;
		out->bytes = pl_cursor_entry(&cursor).value;
	}
	else
	{
		out->present = false;
		out->bytes = (PLSlice)
		{
			NULL, 0
		};
	}
	return PL_OK;
}

PLStatus
pl_scan(const PLTree *tree, const PLSlice *lower, const PLSlice *upper,
		PLScanFn callback, void *arg)
{
	PLCursor	cursor;
	PLStatus	status;

	if (!callback || (upper && !valid_slice(*upper)))
		return PL_INVALID;
	status = pl_cursor_init(&cursor, tree, lower);
	while (status == PL_OK && cursor.valid)
	{
		PLEntry		entry = pl_cursor_entry(&cursor);

		if (upper && pl_compare(entry.key, *upper) >= 0)
			break;
		status = callback(arg, entry.key, entry.value);
		if (status == PL_OK)
			status = pl_cursor_next(&cursor);
	}
	return status;
}

/*
 * Check the subtree under |node|, all of whose keys must exceed |lower|. With
 * |trust_backed|, a backed child's subtree is taken as valid once it fits.
 */
static PLStatus
validate_node(PLStore *store, PLNode *node, const PLSlice *lower, bool trust_backed, uint64_t *rows)
{
	uint32_t	i;

	*rows = 0;
	if (lower && (!node->count || pl_compare(pl_node_key(node, 0), *lower) <= 0))
		return PL_CORRUPT;
	if (!node->level)
	{
		*rows = node->count;
		return PL_OK;
	}
	for (i = 0; i < node->count; i++)
	{
		PLSlice		value = pl_node_value(node, i),
					upper = pl_node_key(node, i);
		PLHash		hash;
		PLNode	   *child;
		uint64_t	child_rows,
					expected_rows;
		PLStatus	status;
		PLSlice		previous;
		const PLSlice *child_lower = lower;

		if (i)
		{
			previous = pl_node_key(node, i - 1);
			child_lower = &previous;
		}
		if (!pl_child_decode(value, &hash, &expected_rows))
			return PL_CORRUPT;
		child = pl_find(store, hash);
		if (!child || child->level + 1 != node->level || !child->count ||
			pl_compare(pl_node_key(child, child->count - 1), upper) != 0)
			return PL_CORRUPT;
		if (trust_backed && child->backed)
		{
			status = child_lower && pl_compare(pl_node_key(child, 0), *child_lower) <= 0 ?
				PL_CORRUPT : PL_OK;
			child_rows = pl_node_rows(child);
		}
		else
			status = validate_node(store, child, child_lower, trust_backed, &child_rows);
		if (status != PL_OK || child_rows != expected_rows ||
			UINT64_MAX - *rows < child_rows)
			return PL_CORRUPT;
		*rows += child_rows;
	}
	return PL_OK;
}

static PLStatus
validate_tree(const PLTree *tree, bool trust_backed)
{
	PLNode	   *root;
	uint64_t	rows;

	if (!tree || !tree->store)
		return PL_INVALID;
	root = pl_find(tree->store, tree->root);
	if (!root)
		return PL_CORRUPT;
	if (trust_backed && root->backed)
		return PL_OK;
	return validate_node(tree->store, root, NULL, trust_backed, &rows);
}

PLStatus
pl_validate(const PLTree *tree)
{
	return validate_tree(tree, false);
}

static PLStatus
open_tree(PLStore *store, PLHash root, bool trust_backed, PLTree *out)
{
	PLTree		candidate = {store, root};
	PLStatus	status;

	if (!out)
		return PL_INVALID;
	status = validate_tree(&candidate, trust_backed);
	if (status == PL_OK)
		*out = candidate;
	return status;
}

PLStatus
pl_open(PLStore *store, PLHash root, PLTree *out)
{
	return open_tree(store, root, false, out);
}

PLStatus
pl_open_imported(PLStore *store, PLHash root, PLTree *out)
{
	return open_tree(store, root, true, out);
}

uint64_t
pl_count(const PLTree *tree)
{
	PLNode	   *node = tree ? pl_find(tree->store, tree->root) : NULL;

	return node ? pl_node_rows(node) : 0;
}
