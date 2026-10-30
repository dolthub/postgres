/* Shared-subtree-aware diff and row-level three-way merge. */
#include "internal.h"

static bool
same_value(PLValue a, PLValue b)
{
	return a.present == b.present && (!a.present || pl_compare(a.bytes, b.bytes) == 0);
}

static PLValue
present(PLSlice value)
{
	PLValue		result = {true, value};

	return result;
}

/*
 * Diff two trees by comparing unexpanded subtree references. Each side is a
 * stack of items in key order: a reference to a node not yet read, or a leaf
 * entry. Equal references are skipped without reading them; a reference is
 * read and replaced by its children only when it differs from the other side.
 * Higher references expand first so both sides meet at the same level. The
 * cost follows the changed region, not the tree size.
 */
typedef struct DiffItem
{
	bool		entry;
	uint32_t	level;			/* for references: the referenced node's level */
	PLHash		hash;
	PLSlice		key;
	PLSlice		value;
} DiffItem;

typedef struct DiffStack
{
	PLStore    *store;
	DiffItem   *items;
	size_t		used;
	size_t		capacity;
} DiffStack;

static PLStatus
diff_push(DiffStack *stack, DiffItem item)
{
	if (stack->used == stack->capacity)
	{
		size_t		capacity = stack->capacity ? stack->capacity * 2 : 64;
		DiffItem   *items = realloc(stack->items, capacity * sizeof(DiffItem));

		if (!items)
			return PL_NOMEM;
		stack->items = items;
		stack->capacity = capacity;
	}
	stack->items[stack->used++] = item;
	return PL_OK;
}

/* Replace the reference on top with the node's children, first child on top. */
static PLStatus
diff_expand(DiffStack *stack)
{
	DiffItem	top = stack->items[--stack->used];
	PLNode	   *node = pl_find(stack->store, top.hash);
	PLStatus	status = PL_OK;

	if (!node || node->level != top.level)
		return PL_CORRUPT;
	for (uint32_t i = node->count; i-- > 0 && status == PL_OK;)
	{
		DiffItem	item = {0};

		item.key = pl_node_key(node, i);
		if (node->level)
		{
			item.level = node->level - 1;
			item.hash = pl_child_hash(node, i);
		}
		else
		{
			item.entry = true;
			item.value = pl_node_value(node, i);
		}
		status = diff_push(stack, item);
	}
	return status;
}

PLStatus
pl_diff(const PLTree *before, const PLTree *after, PLDiffFn callback, void *arg)
{
	DiffStack	a = {0},
				b = {0};
	PLStatus	status;
	PLNode	   *ra,
			   *rb;

	if (!before || !after || !before->store || !after->store || !callback)
		return PL_INVALID;
	ra = pl_find(before->store, before->root);
	rb = pl_find(after->store, after->root);
	if (!ra || !rb)
		return PL_CORRUPT;
	a.store = before->store;
	b.store = after->store;
	status = diff_push(&a, (DiffItem) {.level = ra->level,.hash = ra->hash});
	if (status == PL_OK)
		status = diff_push(&b, (DiffItem) {.level = rb->level,.hash = rb->hash});
	while (status == PL_OK && (a.used || b.used))
	{
		DiffItem   *x = a.used ? &a.items[a.used - 1] : NULL;
		DiffItem   *y = b.used ? &b.items[b.used - 1] : NULL;
		PLValue		absent = {false, {NULL, 0}};
		int			comparison;

		if (x && y && !x->entry && !y->entry && pl_hash_equal(x->hash, y->hash))
		{
			before->store->stats.skipped++;
			a.used--;
			b.used--;
			continue;
		}
		if (x && !x->entry && (!y || y->entry || x->level >= y->level))
		{
			status = diff_expand(&a);
			continue;
		}
		if (y && !y->entry)
		{
			status = diff_expand(&b);
			continue;
		}
		comparison = !x ? 1 : !y ? -1 : pl_compare(x->key, y->key);
		if (comparison < 0)
			status = callback(arg, x->key, present(x->value), absent);
		else if (comparison > 0)
			status = callback(arg, y->key, absent, present(y->value));
		else if (pl_compare(x->value, y->value) != 0)
			status = callback(arg, x->key, present(x->value), present(y->value));
		if (comparison <= 0)
			a.used--;
		if (comparison >= 0)
			b.used--;
	}
	free(a.items);
	free(b.items);
	return status;
}

typedef struct PLChanges
{
	PLEdit	   *edits;
	size_t		used;
	size_t		capacity;
} PLChanges;

static PLStatus
record_change(void *arg, PLSlice key, PLValue before, PLValue after)
{
	PLChanges  *changes = arg;

	(void) before;
	if (changes->used == changes->capacity)
	{
		size_t		capacity = changes->capacity ? changes->capacity * 2 : 16;
		PLEdit	   *edits;

		if (capacity < changes->capacity || capacity > SIZE_MAX / sizeof(*edits))
			return PL_NOMEM;
		edits = realloc(changes->edits, capacity * sizeof(*edits));
		if (!edits)
			return PL_NOMEM;
		changes->edits = edits;
		changes->capacity = capacity;
	}
	changes->edits[changes->used++] = (PLEdit)
	{
		key, after
	};
	return PL_OK;
}

PLStatus
pl_merge(const PLTree *base, const PLTree *left, const PLTree *right,
		 PLResolveFn resolver, void *arg, PLTree *out)
{
	PLChanges	a = {0}, b = {0}, result = {0};
	PLStatus	status;
	uint8_t   **owned = NULL;
	size_t		i = 0,
				j = 0,
				allocated = 0;

	if (!base || !left || !right || !out || !base->store ||
		base->store != left->store || base->store != right->store)
		return PL_INVALID;
	if (!pl_find(base->store, base->root) || !pl_find(left->store, left->root) ||
		!pl_find(right->store, right->root))
		return PL_CORRUPT;
	if (pl_hash_equal(left->root, right->root) || pl_hash_equal(base->root, right->root))
	{
		*out = *left;
		return PL_OK;
	}
	if (pl_hash_equal(base->root, left->root))
	{
		*out = *right;
		return PL_OK;
	}
	status = pl_diff(base, left, record_change, &a);
	if (status == PL_OK)
		status = pl_diff(base, right, record_change, &b);
	if (status != PL_OK)
		goto done;
	if (a.used > SIZE_MAX - b.used || a.used + b.used > SIZE_MAX / sizeof(*owned))
	{
		status = PL_NOMEM;
		goto done;
	}
	if (a.used + b.used)
	{
		owned = calloc(a.used + b.used, sizeof(*owned));
		if (!owned)
		{
			status = PL_NOMEM;
			goto done;
		}
	}

	while (status == PL_OK && (i < a.used || j < b.used))
	{
		PLValue		absent = {false, {NULL, 0}};
		int			comparison = i == a.used ? 1 : j == b.used ? -1 :
			pl_compare(a.edits[i].key, b.edits[j].key);
		PLEdit		chosen;

		if (comparison < 0)
			chosen = a.edits[i++];
		else if (comparison > 0)
			chosen = b.edits[j++];
		else
		{
			PLValue		av = a.edits[i].value,
						bv = b.edits[j].value;

			chosen = a.edits[i];
			if (!same_value(av, bv))
			{
				PLValue		original,
							resolved = absent;

				if (!resolver)
				{
					status = PL_CONFLICT;
					break;
				}
				status = pl_get(base, chosen.key, &original);
				if (status == PL_OK)
					status = resolver(arg, chosen.key, original, av, bv, &resolved);
				if (status != PL_OK)
					break;
				if (resolved.present && (resolved.bytes.len > PL_MAX_ITEM ||
										 (resolved.bytes.len && !resolved.bytes.data)))
				{
					status = PL_INVALID;
					break;
				}
				chosen.value = resolved;
				if (resolved.present && resolved.bytes.len)
				{
					uint8_t    *copy = malloc(resolved.bytes.len);

					if (!copy)
					{
						status = PL_NOMEM;
						break;
					}
					memcpy(copy, resolved.bytes.data, resolved.bytes.len);
					owned[allocated++] = copy;
					chosen.value.bytes.data = copy;
				}
			}
			i++;
			j++;
		}
		status = record_change(&result, chosen.key, absent, chosen.value);
	}
	if (status == PL_OK)
		status = pl_apply(base, result.edits, result.used, out);
done:
	for (i = 0; i < allocated; i++)
		free(owned[i]);
	free(owned);
	free(a.edits);
	free(b.edits);
	free(result.edits);
	return status;
}
