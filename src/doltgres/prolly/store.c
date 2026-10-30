/* Immutable, hash-addressed memory chunk store; explicit endian-safe encoding. */
#ifdef PL_POSTGRES
#include "postgres.h"
#include "common/cryptohash.h"
#else
#include <openssl/evp.h>
#endif
#include "internal.h"
#include <limits.h>
#include <unistd.h>

static const uint8_t node_magic[8] = PL_NODE_FORMAT;

uint32_t
pl_read32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
		((uint32_t) p[2] << 8) | p[3];
}

uint64_t
pl_read64(const uint8_t *p)
{
	return ((uint64_t) pl_read32(p) << 32) | pl_read32(p + 4);
}

void
pl_write32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

void
pl_write64(uint8_t *p, uint64_t v)
{
	pl_write32(p, (uint32_t) (v >> 32));
	pl_write32(p + 4, (uint32_t) v);
}

bool
pl_hash_bytes(PLSlice bytes, PLHash *out)
{
	uint8_t		digest[PL_SHA256_SIZE];
#ifdef PL_POSTGRES
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	bool		ok;

	if (!ctx)
		return false;
	ok = pg_cryptohash_init(ctx) == 0 &&
		pg_cryptohash_update(ctx, bytes.data, bytes.len) == 0 &&
		pg_cryptohash_final(ctx, digest, sizeof(digest)) == 0;
	pg_cryptohash_free(ctx);
	if (!ok)
		return false;
#else
	unsigned int length = 0;

	if (EVP_Digest(bytes.data, bytes.len, digest, &length, EVP_sha256(), NULL) != 1 ||
		length != PL_SHA256_SIZE)
		return false;
#endif
	memcpy(out->bytes, digest, PL_HASH_SIZE);
	return true;
}

/* Dolt's big-endian, unpadded base-32 alphabet; 20 bytes = 32 digits. */
void
pl_hash_format(PLHash hash, char out[PL_HASH_TEXT_SIZE + 1])
{
	static const char alphabet[] = "0123456789abcdefghijklmnopqrstuv";
	int			i;

	for (i = 0; i < PL_HASH_TEXT_SIZE; i++)
	{
		int			bit = i * 5;
		int			byte = bit / 8;
		unsigned int pair = (unsigned int) hash.bytes[byte] << 8;

		if (byte + 1 < PL_HASH_SIZE)
			pair |= hash.bytes[byte + 1];
		out[i] = alphabet[(pair >> (11 - bit % 8)) & 31];
	}
	out[PL_HASH_TEXT_SIZE] = '\0';
}

bool
pl_hash_parse(const char *text, PLHash *out)
{
	PLHash		result = {{0}};
	int			i;

	if (strlen(text) != PL_HASH_TEXT_SIZE)
		return false;
	for (i = 0; i < PL_HASH_TEXT_SIZE; i++)
	{
		unsigned char c = (unsigned char) text[i];
		unsigned int value;
		int			bit = i * 5;
		int			byte = bit / 8;

		if (c >= '0' && c <= '9')
			value = c - '0';
		else if (c >= 'a' && c <= 'v')
			value = c - 'a' + 10;
		else
			return false;
		value <<= 11 - bit % 8;
		result.bytes[byte] |= value >> 8;
		if (byte + 1 < PL_HASH_SIZE)
			result.bytes[byte + 1] |= value & 255;
	}
	*out = result;
	return true;
}

int
pl_compare(PLSlice a, PLSlice b)
{
	size_t		common = a.len < b.len ? a.len : b.len;
	int			cmp = common ? memcmp(a.data, b.data, common) : 0;

	return cmp ? cmp : (a.len > b.len) - (a.len < b.len);
}

bool
pl_hash_equal(PLHash a, PLHash b)
{
	return memcmp(a.bytes, b.bytes, PL_HASH_SIZE) == 0;
}

const char *
pl_status_string(PLStatus status)
{
	static const char *const names[] = {"ok", "not found", "invalid input",
	"out of memory", "corrupt content", "merge conflict", "stopped", "I/O error", "store busy"};

	return (unsigned) status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

PLStore *
pl_store_create(void)
{
	PLStore    *store = calloc(1, sizeof(*store));

	if (!store)
		return NULL;
	store->fd = -1;
	store->capacity = 256;
	store->buckets = calloc(store->capacity, sizeof(*store->buckets));
	if (!store->buckets)
	{
		free(store);
		return NULL;
	}
	return store;
}

/* Free a node unlinked from |store|, returning its data if lent. */
static void
free_node(PLStore *store, PLNode *node)
{
	if (node->loan)
		store->give_back(store->lend_arg, node->loan);
	else
		free(node->data);
	free(node);
}

/* Unlink |node| from its bucket and free it. */
static void
remove_node(PLStore *store, PLNode *node)
{
	PLNode	  **link = &store->buckets[pl_read64(node->hash.bytes) % store->capacity];

	while (*link != node)
		link = &(*link)->next;
	*link = node->next;
	store->stats.chunks--;
	store->stats.bytes -= node->size;
	if (node->loan)
		store->stats.borrowed--;
	free_node(store, node);
}

void
pl_store_free(PLStore *store)
{
	size_t		i;

	if (!store)
		return;
	for (i = 0; i < store->capacity; i++)
	{
		PLNode	   *node = store->buckets[i];

		while (node)
		{
			PLNode	   *next = node->next;

			free_node(store, node);
			node = next;
		}
	}
	if (store->fd >= 0)
		close(store->fd);
	free(store->buckets);
	free(store->unbacked);
	free(store->loaned);
	free(store);
}

/* Free the nodes not known to be in the backing store: a transaction's own. */
size_t
pl_store_drop_unbacked(PLStore *store)
{
	size_t		freed = 0;

	if (!store)
		return 0;
	for (size_t i = 0; i < store->unbacked_count; i++)
	{
		PLNode	   *node = store->unbacked[i];

		if (node->backed)
			continue;
		remove_node(store, node);
		freed++;
	}
	store->unbacked_count = 0;
	if (store->unbacked_capacity > 4096)
	{
		free(store->unbacked);
		store->unbacked = NULL;
		store->unbacked_capacity = 0;
	}
	return freed;
}

static PLStatus store_resize(PLStore *store, size_t capacity);

size_t
pl_store_drop_backed(PLStore *store)
{
	size_t		freed = 0,
				capacity;

	if (!store)
		return 0;

	/* Nodes written since they were listed as unbacked are about to be freed: forget them. */
	{
		size_t		kept = 0;

		for (size_t i = 0; i < store->unbacked_count; i++)
			if (!store->unbacked[i]->backed)
			{
				store->unbacked[i]->unbacked_slot = kept;
				store->unbacked[kept++] = store->unbacked[i];
			}
		store->unbacked_count = kept;
	}
	for (size_t i = 0; i < store->capacity; i++)
	{
		PLNode	  **link = &store->buckets[i];

		while (*link)
		{
			PLNode	   *node = *link;

			if (!node->backed || node->loan)
			{
				link = &node->next;
				continue;
			}
			*link = node->next;
			store->stats.chunks--;
			store->stats.bytes -= node->size;
			free_node(store, node);
			freed++;
		}
	}

	/* Shrink, so that later drops walk few buckets. */
	for (capacity = store->capacity; capacity > 256 && store->stats.chunks < capacity / 8; capacity /= 2)
		;
	if (capacity < store->capacity)
		store_resize(store, capacity);
	return freed;
}

PLStats
pl_store_stats(const PLStore *store)
{
	return store->stats;
}

static void
mark_reachable(PLStore *store, PLHash hash)
{
	PLNode	   *node = pl_find_local(store, hash);

	if (!node || node->marked)
		return;
	node->marked = true;
	for (uint32_t i = 0; node->level && i < node->count; i++)
	{
		PLHash		child;

		child = pl_child_hash(node, i);
		mark_reachable(store, child);
	}
}

size_t
pl_store_retain(PLStore *store, const PLHash *roots, size_t count)
{
	size_t		freed = 0;

	if (!store)
		return 0;
	for (size_t i = 0; i < count; i++)
		mark_reachable(store, roots[i]);

	/*
	 * A pinned node is kept, with what it reaches, whatever the roots: its
	 * holders remember it by hash and would take a rebuilt copy of it for
	 * the one they pinned.
	 */
	for (size_t i = 0; i < store->capacity; i++)
		for (PLNode *node = store->buckets[i]; node; node = node->next)
			if (node->pins > 0 && !node->marked)
				mark_reachable(store, node->hash);

	/* Unreachable nodes are about to be freed: forget them first. */
	{
		size_t		kept = 0;

		for (size_t i = 0; i < store->unbacked_count; i++)
			if (store->unbacked[i]->marked)
			{
				store->unbacked[i]->unbacked_slot = kept;
				store->unbacked[kept++] = store->unbacked[i];
			}
		store->unbacked_count = kept;
		kept = 0;
		for (size_t i = 0; i < store->loaned_count; i++)
			if (store->loaned[i]->marked)
				store->loaned[kept++] = store->loaned[i];
		store->loaned_count = kept;
	}
	for (size_t i = 0; i < store->capacity; i++)
	{
		PLNode	  **link = &store->buckets[i];

		while (*link)
		{
			PLNode	   *node = *link;

			if (node->marked)
			{
				node->marked = false;
				link = &node->next;
				continue;
			}
			*link = node->next;
			store->stats.chunks--;
			store->stats.bytes -= node->size;
			if (node->loan)
				store->stats.borrowed--;
			free_node(store, node);
			freed++;
		}
	}
	return freed;
}

size_t
pl_store_return_loans(PLStore *store)
{
	size_t		returned;

	if (!store)
		return 0;
	returned = store->loaned_count;
	for (size_t i = 0; i < returned; i++)
		remove_node(store, store->loaned[i]);
	store->loaned_count = 0;
	if (store->loaned_capacity > 4096)
	{
		free(store->loaned);
		store->loaned = NULL;
		store->loaned_capacity = 0;
	}
	return returned;
}

/*
 * Keep |node| through pl_store_drop_unbacked until it is written: for a node
 * every transaction would otherwise rebuild, such as the empty tree. Pins
 * count: each holder releases its own.
 */
void
pl_node_pin(PLStore *store, PLNode *node)
{
	size_t		i;

	if (node->backed || node->pins++ > 0)
		return;
	i = node->unbacked_slot;
	if (i >= store->unbacked_count || store->unbacked[i] != node)
	{
		for (i = 0; i < store->unbacked_count && store->unbacked[i] != node; i++)
			;
		if (i == store->unbacked_count)
			return;
	}
	store->unbacked[i] = store->unbacked[--store->unbacked_count];
	store->unbacked[i]->unbacked_slot = i;
}

/*
 * Release one pin of |node|; with the last, it drops with the next
 * pl_store_drop_unbacked unless written by then.
 */
void
pl_node_unpin(PLStore *store, PLNode *node)
{
	if (node->pins == 0 || --node->pins > 0)
		return;
	if (node->backed)
		return;
	if (store->unbacked_count == store->unbacked_capacity)
	{
		size_t		capacity = store->unbacked_capacity ? store->unbacked_capacity * 2 : 64;
		PLNode	  **grown = realloc(store->unbacked, capacity * sizeof(*grown));

		if (!grown)
			return;				/* kept until the store is freed */
		store->unbacked = grown;
		store->unbacked_capacity = capacity;
	}
	node->unbacked_slot = store->unbacked_count;
	store->unbacked[store->unbacked_count++] = node;
}

/* Record that |node| is now in the backing store; it stays listed until dropped. */
void
pl_node_set_backed(PLNode *node)
{
	node->backed = true;
}

/* Find a node already in this store, without consulting a backing store. */
PLNode *
pl_find_local(PLStore *store, PLHash hash)
{
	PLNode	   *node;

	if (!store)
		return NULL;
	for (node = store->buckets[pl_read64(hash.bytes) % store->capacity];
		 node; node = node->next)
		if (pl_hash_equal(node->hash, hash))
			return node;
	return NULL;
}

static PLStatus store_buffer(PLStore *store, PLHash hash, const uint8_t *bytes, size_t length, bool backed, PLNode **out);
static PLStatus store_loan(PLStore *store, PLHash hash, PLLoan loan, PLNode **out);

PLNode *
pl_find(PLStore *store, PLHash hash)
{
	PLNode	   *node = pl_find_local(store, hash);
	PLSlice		bytes;

	if (node || !store)
		return node;
	if (store->lend)
	{
		PLLoan		loan = {{NULL, 0}, 0};

		if (store->lend(store->lend_arg, hash, &loan) != PL_OK)
			return NULL;
		if (loan.token ? store_loan(store, hash, loan, &node) != PL_OK :
			store_buffer(store, hash, loan.bytes.data, loan.bytes.len, true, &node) != PL_OK)
			return NULL;
		store->stats.fetched++;
		return node;
	}
	if (!store->fetch)
		return NULL;
	if (store->fetch(store->fetch_arg, hash, &bytes) != PL_OK ||
		store_buffer(store, hash, bytes.data, bytes.len, true, &node) != PL_OK)
		return NULL;
	store->stats.fetched++;
	return node;
}

void
pl_store_set_fetch(PLStore *store, PLFetchFn fetch, void *arg)
{
	store->fetch = fetch;
	store->fetch_arg = arg;
}

void
pl_store_set_lender(PLStore *store, PLLendFn lend, PLReturnFn give_back, void *arg)
{
	store->lend = lend;
	store->give_back = give_back;
	store->lend_arg = arg;
}

/* The width of a directory offset for keys and values of these sizes. */
static size_t
offset_width(uint64_t key_bytes, uint64_t value_bytes)
{
	return key_bytes <= PL_NARROW_LIMIT && value_bytes <= PL_NARROW_LIMIT ? 2 : 4;
}

static uint32_t
read_offset(const uint8_t *p, size_t width)
{
	return width == 2 ? ((uint32_t) p[0] << 8) | p[1] : pl_read32(p);
}

static void
write_offset(uint8_t *p, size_t width, uint32_t v)
{
	if (width == 2)
	{
		p[0] = (uint8_t) (v >> 8);
		p[1] = (uint8_t) v;
	}
	else
		pl_write32(p, v);
}

static size_t
directory_size(uint32_t count, size_t width)
{
	return 2 * width * ((size_t) count + 1);
}

PLSlice
pl_node_key(const PLNode *node, uint32_t index)
{
	size_t		width = node->offset_width;
	const uint8_t *offsets = node->data + PL_HEADER;
	uint32_t	start = read_offset(offsets + width * index, width);
	PLSlice		result = {node->data + PL_HEADER + directory_size(node->count, width) + start,
	read_offset(offsets + width * (index + 1), width) - start};

	return result;
}

PLSlice
pl_node_value(const PLNode *node, uint32_t index)
{
	size_t		width = node->offset_width;
	const uint8_t *offsets = node->data + PL_HEADER + width * (node->count + 1);
	uint32_t	start = read_offset(offsets + width * index, width);
	PLSlice		result = {node->data + PL_HEADER + directory_size(node->count, width) +
	node->key_bytes + start, read_offset(offsets + width * (index + 1), width) - start};

	return result;
}

/* The address of node bytes: their hash, like any other content's. */
bool
pl_node_address(PLSlice bytes, PLHash *out)
{
	return pl_hash_bytes(bytes, out);
}

size_t
pl_child_encode(uint8_t *out, PLHash hash, uint64_t rows)
{
	size_t		n = PL_HASH_SIZE;

	memcpy(out, hash.bytes, PL_HASH_SIZE);
	do
	{
		uint8_t		byte = rows & 0x7F;

		rows >>= 7;
		out[n++] = byte | (rows ? 0x80 : 0);
	} while (rows);
	return n;
}

bool
pl_child_decode(PLSlice value, PLHash *hash, uint64_t *rows)
{
	uint64_t	count = 0;
	size_t		n = PL_HASH_SIZE;
	int			shift = 0;

	if (value.len <= PL_HASH_SIZE || value.len > PL_CHILD_MAX)
		return false;
	if (hash)
		memcpy(hash->bytes, value.data, PL_HASH_SIZE);
	for (;;)
	{
		uint8_t		byte = value.data[n++];

		count |= (uint64_t) (byte & 0x7F) << shift;
		if (!(byte & 0x80))
			break;
		shift += 7;
		if (n == value.len || shift > 63)
			return false;
	}
	if (n != value.len)
		return false;
	if (rows)
		*rows = count;
	return true;
}

/*
 * A child's address and row count, read without checks: a node's child
 * references were validated by pl_child_decode when the node was parsed.
 */
PLHash
pl_child_hash(const PLNode *node, uint32_t index)
{
	PLHash		hash;

	memcpy(hash.bytes, pl_node_value(node, index).data, PL_HASH_SIZE);
	return hash;
}

uint64_t
pl_child_rows(const PLNode *node, uint32_t index)
{
	const uint8_t *p = pl_node_value(node, index).data + PL_HASH_SIZE;
	uint64_t	rows = 0;
	int			shift = 0;

	do
	{
		rows |= (uint64_t) (*p & 0x7F) << shift;
		shift += 7;
	} while (*p++ & 0x80);
	return rows;
}

uint64_t
pl_node_rows(const PLNode *node)
{
	uint64_t	rows = 0;
	uint32_t	i;

	if (node->level == 0)
		return node->count;
	for (i = 0; i < node->count; i++)
		rows += pl_child_rows(node, i);
	return rows;
}

/* Decode a node's header; enough to read a node whose encoding is known good. */
static void
parse_header(PLNode *node)
{
	node->level = pl_read32(node->data + 8);
	node->count = pl_read32(node->data + 12);
	node->key_bytes = pl_read32(node->data + 16);
	node->value_bytes = pl_read32(node->data + 20);
	node->offset_width = (uint8_t) offset_width(node->key_bytes, node->value_bytes);
}

/* Decode metadata only after all offsets have been checked against buffer bounds. */
static PLStatus
parse_node(PLNode *node)
{
	uint32_t	i,
				which;
	size_t		directory,
				width;
	uint64_t	rows = 0;

	if (node->size < PL_HEADER || memcmp(node->data, node_magic, 8) != 0)
		return PL_CORRUPT;
	parse_header(node);
	if (node->level >= PL_MAX_DEPTH || node->count > PL_MAX_ENTRIES ||
		(node->level && !node->count))
		return PL_CORRUPT;
	width = node->offset_width;
	directory = PL_HEADER + directory_size(node->count, width);
	if (directory + (uint64_t) node->key_bytes + node->value_bytes != node->size)
		return PL_CORRUPT;
	for (which = 0; which < 2; which++)
	{
		const uint8_t *offsets = node->data + PL_HEADER + which * width * (node->count + 1);
		uint32_t	limit = which ? node->value_bytes : node->key_bytes;

		if (read_offset(offsets, width) != 0 || read_offset(offsets + width * node->count, width) != limit)
			return PL_CORRUPT;
		for (i = 0; i < node->count; i++)
		{
			uint32_t	start = read_offset(offsets + width * i, width);
			uint32_t	end = read_offset(offsets + width * (i + 1), width);

			if (end < start || end > limit || end - start > PL_MAX_ITEM ||
				(which && node->level && (end - start <= PL_HASH_SIZE || end - start > PL_CHILD_MAX)))
				return PL_CORRUPT;
		}
	}
	for (i = 0; i < node->count; i++)
	{
		if (i && pl_compare(pl_node_key(node, i - 1), pl_node_key(node, i)) >= 0)
			return PL_CORRUPT;
		if (node->level)
		{
			uint64_t	child_rows;

			if (!pl_child_decode(pl_node_value(node, i), NULL, &child_rows))
				return PL_CORRUPT;
			if (!child_rows || UINT64_MAX - rows < child_rows)
				return PL_CORRUPT;
			rows += child_rows;
		}
	}
	return PL_OK;
}

bool
pl_node_check(PLSlice bytes)
{
	PLNode		node = {0};

	if (!bytes.data)
		return false;
	node.data = (uint8_t *) bytes.data;
	node.size = bytes.len;
	return parse_node(&node) == PL_OK;
}

/* Rehash |store| into |capacity| buckets. */
static PLStatus
store_resize(PLStore *store, size_t capacity)
{
	PLNode	  **buckets = calloc(capacity, sizeof(*buckets));

	if (!buckets)
		return PL_NOMEM;
	for (size_t i = 0; i < store->capacity; i++)
	{
		PLNode	   *current = store->buckets[i];

		while (current)
		{
			PLNode	   *next = current->next;
			size_t		bucket = pl_read64(current->hash.bytes) % capacity;

			current->next = buckets[bucket];
			buckets[bucket] = current;
			current = next;
		}
	}
	free(store->buckets);
	store->buckets = buckets;
	store->capacity = capacity;
	return PL_OK;
}

/* Make room to add a node to |store|: buckets for it, and a list slot if unbacked. */
static PLStatus
store_reserve(PLStore *store, bool backed)
{
	if (store->stats.chunks >= store->capacity * 2 &&
		store_resize(store, store->capacity * 2) != PL_OK)
		return PL_NOMEM;
	if (!backed && store->unbacked_count == store->unbacked_capacity)
	{
		size_t		capacity = store->unbacked_capacity ? store->unbacked_capacity * 2 : 64;
		PLNode	  **grown = realloc(store->unbacked, capacity * sizeof(*grown));

		if (!grown)
			return PL_NOMEM;
		store->unbacked = grown;
		store->unbacked_capacity = capacity;
	}
	return PL_OK;
}

/* Add a node to |store|, after store_reserve. */
static void
store_link(PLStore *store, PLNode *node, bool backed)
{
	size_t		bucket = pl_read64(node->hash.bytes) % store->capacity;

	node->next = store->buckets[bucket];
	store->buckets[bucket] = node;
	store->stats.chunks++;
	store->stats.bytes += node->size;
	node->backed = backed;
	if (!backed)
	{
		node->unbacked_slot = store->unbacked_count;
		store->unbacked[store->unbacked_count++] = node;
	}
}

/* Add a node reading lent bytes in place; the loan is returned on failure. */
static PLStatus
store_loan(PLStore *store, PLHash hash, PLLoan loan, PLNode **out)
{
	PLNode	   *node = NULL;
	PLStatus	status = PL_NOMEM;

	if (store->loaned_count == store->loaned_capacity)
	{
		size_t		capacity = store->loaned_capacity ? store->loaned_capacity * 2 : 64;
		PLNode	  **grown = realloc(store->loaned, capacity * sizeof(*grown));

		if (!grown)
			goto fail;
		store->loaned = grown;
		store->loaned_capacity = capacity;
	}
	node = calloc(1, sizeof(*node));
	if (!node || (status = store_reserve(store, true)) != PL_OK)
		goto fail;
	node->data = (uint8_t *) loan.bytes.data;
	node->size = loan.bytes.len;
	node->hash = hash;
	node->loan = loan.token;
	parse_header(node);
	store_link(store, node, true);
	store->loaned[store->loaned_count++] = node;
	store->stats.borrowed++;
	*out = node;
	return PL_OK;

fail:
	free(node);
	store->give_back(store->lend_arg, loan.token);
	return status;
}

static PLStatus
store_buffer(PLStore *store, PLHash hash, const uint8_t *bytes, size_t length, bool backed, PLNode **out)
{
	PLNode	   *node = pl_find_local(store, hash);
	PLStatus	status;

	if (!store)
		return PL_INVALID;
	if (store->failed)
		return PL_IO;
	if (node)
	{
		if (node->size != length || memcmp(node->data, bytes, length) != 0)
			return PL_CORRUPT;
		store->stats.reused++;
		*out = node;
		return PL_OK;
	}
	node = calloc(1, sizeof(*node));
	if (!node)
		return PL_NOMEM;
	node->data = malloc(length);
	if (!node->data)
	{
		free(node);
		return PL_NOMEM;
	}
	memcpy(node->data, bytes, length);
	node->size = length;
	node->hash = hash;
	if (parse_node(node) != PL_OK)
	{
		free(node->data);
		free(node);
		return PL_CORRUPT;
	}
	status = store_reserve(store, backed);
	if (status == PL_OK && store->fd >= 0 && !store->loading)
	{
		PLSlice		data = {bytes, length};

		status = pl_file_append(store, hash, data);
	}
	if (status != PL_OK)
	{
		free(node->data);
		free(node);
		return status;
	}
	store_link(store, node, backed);
	*out = node;
	return PL_OK;
}

PLStatus
pl_store_export(PLStore *store, PLHash hash, PLSlice *bytes)
{
	PLNode	   *node = pl_find(store, hash);

	if (!bytes || !store)
		return PL_INVALID;
	if (!node)
		return PL_NOT_FOUND;
	bytes->data = node->data;
	bytes->len = node->size;
	return PL_OK;
}

PLStatus
pl_store_import(PLStore *store, PLHash hash, PLSlice bytes)
{
	PLHash		actual;
	PLNode	   *node;

	if (!store || !bytes.data || bytes.len > 2ULL * PL_MAX_ITEM * PL_MAX_ENTRIES + 2048)
		return PL_INVALID;
	if (!pl_node_address(bytes, &actual) || !pl_hash_equal(hash, actual))
		return PL_CORRUPT;
	return store_buffer(store, hash, bytes.data, bytes.len, false, &node);
}

PLStatus
pl_save(PLStore *store, uint32_t level, const PLEntry *entries, uint32_t count, PLNode **out)
{
	size_t		key_bytes = 0,
				value_bytes = 0,
				length,
				start,
				width;
	uint32_t	i;
	uint8_t    *data;
	PLHash		hash;
	PLSlice		bytes;
	PLStatus	status;

	for (i = 0; i < count; i++)
	{
		key_bytes += entries[i].key.len;
		value_bytes += entries[i].value.len;
	}
	if (key_bytes > UINT32_MAX || value_bytes > UINT32_MAX)
		return PL_INVALID;
	width = offset_width(key_bytes, value_bytes);
	length = PL_HEADER + directory_size(count, width) + key_bytes + value_bytes;
	data = calloc(1, length);
	if (!data)
		return PL_NOMEM;
	memcpy(data, node_magic, 8);
	pl_write32(data + 8, level);
	pl_write32(data + 12, count);
	pl_write32(data + 16, (uint32_t) key_bytes);
	pl_write32(data + 20, (uint32_t) value_bytes);
	start = PL_HEADER + directory_size(count, width);
	key_bytes = value_bytes = 0;
	for (i = 0; i < count; i++)
	{
		if (entries[i].key.len)
			memcpy(data + start + key_bytes, entries[i].key.data, entries[i].key.len);
		key_bytes += entries[i].key.len;
		write_offset(data + PL_HEADER + width * (i + 1), width, (uint32_t) key_bytes);
	}
	for (i = 0; i < count; i++)
	{
		if (entries[i].value.len)
			memcpy(data + start + key_bytes + value_bytes, entries[i].value.data, entries[i].value.len);
		value_bytes += entries[i].value.len;
		write_offset(data + PL_HEADER + width * (count + 1) + width * (i + 1), width, (uint32_t) value_bytes);
	}
	bytes.data = data;
	bytes.len = length;
	if (!pl_node_address(bytes, &hash))
	{
		free(data);
		return PL_CORRUPT;
	}
	store->stats.writes++;
	status = store_buffer(store, hash, data, length, false, out);
	free(data);
	return status;
}

bool
pl_boundary(uint32_t level, uint32_t count, size_t bytes, size_t entry_bytes, PLSlice key)
{
	/*
	 * Key-local, level-salted fingerprint; no dependence on insertion
	 * history. An entry ends its node with probability proportional to its
	 * size, so nodes average PL_TARGET_BYTES whatever their entries' sizes.
	 */
	uint64_t	fingerprint = UINT64_C(14695981039346656037) ^ level;
	size_t		i;

	if (count < 2)
		return false;
	if (count >= PL_MAX_ENTRIES || bytes >= PL_MAX_BYTES)
		return true;
	if (bytes < PL_MIN_BYTES)
		return false;
	for (i = 0; i < key.len; i++)
		fingerprint = (fingerprint ^ key.data[i]) * UINT64_C(1099511628211);
	/* Avalanche the integer-key suffix before testing the boundary bits. */
	fingerprint ^= fingerprint >> 33;
	fingerprint *= UINT64_C(0xff51afd7ed558ccd);
	fingerprint ^= fingerprint >> 33;
	return (fingerprint & 0xFFFFF) < (uint64_t) entry_bytes * 0x100000 / PL_TARGET_BYTES;
}
