/* Append-only chunk file. Root publication belongs to the transaction layer. */
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_HEADER 80
static const uint8_t record_magic[8] = {'D', 'G', 'C', 'H', 'N', 'K', '0', '1'};

static bool
header_digest(const uint8_t *header, uint8_t *digest)
{
	PLHash		hash;
	PLSlice		bytes = {header, 48};

	if (!pl_hash_bytes(bytes, &hash))
		return false;
	memcpy(digest, hash.bytes, PL_HASH_SIZE);
	return true;
}

static PLStatus
write_all(int fd, const uint8_t *data, size_t length)
{
	while (length)
	{
		ssize_t		n = write(fd, data, length);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return PL_IO;
		data += n;
		length -= (size_t) n;
	}
	return PL_OK;
}

/* An EOF is distinct from an operating-system read error. */
static PLStatus
read_all(int fd, uint8_t *data, size_t length, size_t *got)
{
	*got = 0;
	while (*got < length)
	{
		ssize_t		n = read(fd, data + *got, length - *got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return PL_IO;
		if (!n)
			break;
		*got += (size_t) n;
	}
	return PL_OK;
}

PLStatus
pl_file_append(PLStore *store, PLHash hash, PLSlice bytes)
{
	uint8_t		header[RECORD_HEADER];
	PLStatus	status;

	if (store->failed)
		return PL_IO;
	if (store->before_write && !store->replaying)
	{
		status = store->before_write(store->write_arg, hash, bytes);
		if (status != PL_OK)
			return status;
	}
	memset(header, 0, sizeof(header));
	memcpy(header, record_magic, 8);
	pl_write64(header + 8, bytes.len);
	memcpy(header + 16, hash.bytes, PL_HASH_SIZE);
	if (!header_digest(header, header + 48))
		return PL_CORRUPT;
	status = write_all(store->fd, header, sizeof(header));
	if (status == PL_OK)
		status = write_all(store->fd, bytes.data, bytes.len);
	if (status != PL_OK)
		store->failed = true;
	return status;
}

PLStatus
pl_store_sync(PLStore *store)
{
	if (!store)
		return PL_INVALID;
	if (store->failed)
		return PL_IO;
	if (store->fd >= 0 && fsync(store->fd) != 0)
	{
		store->failed = true;
		return PL_IO;
	}
	return PL_OK;
}

PLStatus
pl_store_replay(PLStore *store, PLHash hash, PLSlice bytes)
{
	PLStatus	status;

	if (!store)
		return PL_INVALID;
	store->replaying = true;
	status = pl_store_import(store, hash, bytes);
	store->replaying = false;
	return status;
}

PLStatus
pl_store_open_file(const char *path, bool repair_tail,
				   PLBeforeWriteFn before_write, void *arg, PLStore **out)
{
	PLStore    *store;
	PLStatus	status = PL_OK;
	off_t		valid_end = 0;
	char	   *parent,
			   *slash;
	int			directory;
	struct stat info;

	if (!path || !*path || !out)
		return PL_INVALID;
	store = pl_store_create();
	if (!store)
		return PL_NOMEM;
	store->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (store->fd < 0)
	{
		status = PL_IO;
		goto fail;
	}
	if (fstat(store->fd, &info) != 0 || !S_ISREG(info.st_mode))
	{
		status = PL_IO;
		goto fail;
	}
	if (flock(store->fd, LOCK_EX | LOCK_NB) != 0)
	{
		status = (errno == EWOULDBLOCK) ? PL_BUSY : PL_IO;
		goto fail;
	}
	/* Persist the directory entry too, including when reopening a new file. */
	parent = strdup(path);
	if (!parent)
	{
		status = PL_NOMEM;
		goto fail;
	}
	slash = strrchr(parent, '/');
	if (slash)
	{
		if (slash == parent)
			slash[1] = 0;
		else
			*slash = 0;
	}
	else
	{
		free(parent);
		parent = strdup(".");
	}
	if (!parent)
	{
		status = PL_NOMEM;
		goto fail;
	}
	directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	free(parent);
	if (directory < 0)
	{
		status = PL_IO;
		goto fail;
	}
	if (fsync(store->fd) != 0 || fsync(directory) != 0)
		status = PL_IO;
	close(directory);
	if (status != PL_OK)
		goto fail;
	store->loading = true;
	for (;;)
	{
		uint8_t		header[RECORD_HEADER],
				   *data;
		size_t		got;
		uint64_t	length;
		PLHash		hash;
		uint8_t		digest[PL_HASH_SIZE];
		PLSlice		bytes;
		struct stat statbuf;

		status = read_all(store->fd, header, sizeof(header), &got);
		if (status != PL_OK)
			goto fail;
		if (!got)
			break;
		if (got != sizeof(header))
			goto tail;
		if (memcmp(header, record_magic, 8) || !header_digest(header, digest) ||
			memcmp(digest, header + 48, PL_HASH_SIZE))
		{
			status = PL_CORRUPT;
			goto fail;
		}
		length = pl_read64(header + 8);
		if (length < PL_HEADER || length > 2ULL * PL_MAX_ITEM * PL_MAX_ENTRIES + 2048 || length > SIZE_MAX)
		{
			status = PL_CORRUPT;
			goto fail;
		}
		if (fstat(store->fd, &statbuf) != 0)
		{
			status = PL_IO;
			goto fail;
		}
		if ((uint64_t) (statbuf.st_size - valid_end - RECORD_HEADER) < length)
			goto tail;
		data = malloc((size_t) length);
		if (!data)
		{
			status = PL_NOMEM;
			goto fail;
		}
		status = read_all(store->fd, data, (size_t) length, &got);
		if (status != PL_OK)
		{
			free(data);
			goto fail;
		}
		if (got != length)
		{
			free(data);
			goto tail;
		}
		memcpy(hash.bytes, header + 16, PL_HASH_SIZE);
		bytes.data = data;
		bytes.len = (size_t) length;
		status = pl_store_import(store, hash, bytes);
		free(data);
		if (status != PL_OK)
			goto fail;
		valid_end = lseek(store->fd, 0, SEEK_CUR);
		if (valid_end < 0)
		{
			status = PL_IO;
			goto fail;
		}
	}
	store->loading = false;
	store->before_write = before_write;
	store->write_arg = arg;
	*out = store;
	return PL_OK;
tail:
	if (!repair_tail)
	{
		status = PL_CORRUPT;
		goto fail;
	}
	if (ftruncate(store->fd, valid_end) != 0 || fsync(store->fd) != 0 ||
		lseek(store->fd, valid_end, SEEK_SET) < 0)
	{
		status = PL_IO;
		goto fail;
	}
	store->loading = false;
	store->before_write = before_write;
	store->write_arg = arg;
	*out = store;
	return PL_OK;
fail:
	pl_store_free(store);
	return status;
}
