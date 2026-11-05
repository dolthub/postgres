/* Native, self-contained manifest bundles. Runtime OIDs and file locators are
 * rebuilt at the destination, never transported. Included by dg_version.c. */

#define DG_BUNDLE_MAGIC "DGBUNDLE1\n"

typedef struct BundleObject
{
	char key[PL_HASH_SIZE * 2 + 1];
	char *kind;
	bytea *payload;
	ArrayType *children;
	int state;
} BundleObject;

/*
 * Append to |out| the nodes of the tree rooted at |root| that |seen| lacks, as
 * [hex hash, base64 bytes] pairs after |count| others, reading them through
 * |store|.
 */
static void
bundle_tree(StringInfo out, int *count, PLStore *store, HTAB *seen, PLHash root)
{
	PLNode	   *node;
	bool		found;
	char	   *encoded;
	int			length;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	hash_search(seen, root.bytes, HASH_ENTER, &found);
	if (found)
		return;
	node = pl_find(store, root);
	if (!node)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("retained tree is missing from the chunk store")));
	for (uint32 i = 0; node->level && i < node->count; i++)
		bundle_tree(out, count, store, seen, pl_child_hash(node, i));
	length = pg_b64_enc_len(node->size);
	encoded = palloc(length + 1);
	length = pg_b64_encode(node->data, node->size, encoded, length);
	if (length < 0)
		elog(ERROR, "could not encode a bundle chunk");
	encoded[length] = '\0';
	appendStringInfo(out, "%s[\"", (*count)++ ? "," : "");
	for (int i = 0; i < PL_HASH_SIZE; i++)
		appendStringInfo(out, "%02x", root.bytes[i]);
	appendStringInfo(out, "\",\"%s\"]", encoded);
	pfree(encoded);
}

/* Append chunk |hash|, read from |chunks|, as bundle_tree does a node. */
static void
bundle_chunk(StringInfo out, int *count, DoltChunkStore *chunks, const uint8 *hash)
{
	DoltChunk	chunk;
	char	   *encoded;
	int			length;

	if (!dolt_chunk_get(chunks, hash, &chunk))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root is missing from the chunk store")));
	length = pg_b64_enc_len(chunk.length);
	encoded = palloc(length + 1);
	length = pg_b64_encode(chunk.data, chunk.length, encoded, length);
	if (length < 0)
		elog(ERROR, "could not encode a bundle chunk");
	encoded[length] = '\0';
	appendStringInfo(out, "%s[\"", (*count)++ ? "," : "");
	for (int i = 0; i < PL_HASH_SIZE; i++)
		appendStringInfo(out, "%02x", hash[i]);
	appendStringInfo(out, "\",\"%s\"]", encoded);
	pfree(encoded);
	dolt_chunk_release(chunks, &chunk);
}

/* The sequence schemas the roots a bundle carries name, each once. */
static List *bundle_sequences;

typedef struct BundleRoot
{
	StringInfo	out;
	int		   *count;
	HTAB	   *seen;
	DoltChunkStore *chunks;
} BundleRoot;

static void
bundle_root_node(void *arg, const uint8 *hash, int level)
{
	BundleRoot *bundle = arg;
	bool		found;

	hash_search(bundle->seen, hash, HASH_ENTER, &found);
	if (!found)
		bundle_chunk(bundle->out, bundle->count, bundle->chunks, hash);
}

/* Append the nodes of database root |root| and of the content trees it names. */
static void
bundle_root(StringInfo out, int *count, PLStore *store, HTAB *seen, bytea *root)
{
	BundleRoot	bundle = {out, count, seen, dolt_chunk_open(DOLT_CHUNK_READ)};
	DGRootEntry *entries;
	int			n;

	if (hash_search(seen, VARDATA_ANY(root), HASH_FIND, NULL))
		return;
	dg_root_nodes(root, bundle_root_node, &bundle);
	dolt_chunk_close(bundle.chunks);
	entries = dg_root_entries(root, &n);
	for (int i = 0; i < n; i++)
	{
		if (entries[i].has_content)
			bundle_tree(out, count, store, seen, entries[i].content);
		if (!strcmp(entries[i].kind, "sequence"))
		{
			bytea	   *schema = bytes_of_hash(&entries[i].schema);
			bool		listed = false;

			foreach_ptr(bytea, known, bundle_sequences)
				listed |= VARSIZE_ANY_EXHDR(known) == PL_HASH_SIZE && !memcmp(VARDATA_ANY(known), VARDATA_ANY(schema), PL_HASH_SIZE);
			if (!listed)
				bundle_sequences = lappend(bundle_sequences, schema);
		}
	}
}

/*
 * The nodes of every tree and database root the visible objects name, and of
 * the |nroots| roots |roots| refs name, as a JSON array.
 */
static char *
bundle_chunks(bytea **roots, int nroots)
{
	StringInfoData out;
	HASHCTL		control = {0};
	HTAB	   *seen;
	PLStore    *store = dg_private_store();
	int			count = 0;
	int			n;
	char	  **kinds;
	ArrayType **children;

	control.keysize = PL_HASH_SIZE;
	control.entrysize = PL_HASH_SIZE;
	seen = hash_create("bundle chunks", 1024, &control, HASH_ELEM | HASH_BLOBS);
	run("SELECT objkind,objchildren FROM pg_dolt_object_visible WHERE cardinality(objchildren)>0", 0, NULL, NULL, NULL, true);
	n = SPI_processed;
	kinds = palloc(sizeof(char *) * Max(n, 1));
	children = palloc(sizeof(ArrayType *) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		kinds[i] = strcell(i, 1);
		children[i] = DatumGetArrayTypePCopy(cell(i, 2));
	}
	initStringInfo(&out);
	appendStringInfoChar(&out, '[');
	for (int i = 0; i < n; i++)
	{
		Datum	   *values;
		bool	   *nulls;
		int			count_children;

		deconstruct_array(children[i], BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &count_children);
		for (int c = 0; c < count_children; c++)
		{
			bytea	   *edge = DatumGetByteaPP(values[c]);
			PLHash		root;

			if (dg_manifest_root_edge(kinds[i], c))
			{
				bundle_root(&out, &count, store, seen, edge);
				continue;
			}
			if (!dg_manifest_tree_edge(kinds[i], c))
				continue;
			if (VARSIZE_ANY_EXHDR(edge) != PL_HASH_SIZE)
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid retained tree address")));
			memcpy(root.bytes, VARDATA_ANY(edge), PL_HASH_SIZE);
			bundle_tree(&out, &count, store, seen, root);
		}
	}
	for (int i = 0; i < nroots; i++)
		bundle_root(&out, &count, store, seen, roots[i]);
	appendStringInfoChar(&out, ']');
	hash_destroy(seen);
	pl_store_free(store);
	return out.data;
}

/* Put the chunks of bundle |payload| that the chunk store lacks, checking each. */
static void
bundle_import_chunks(const char *payload)
{
	int			n;
	uint8	   *hashes;
	const uint8 **data;
	Size	   *lengths;
	bool	   *found;
	int			missing = 0;
	DoltChunkStore *chunks;

	run("SELECT decode(v->>0,'hex'),decode(v->>1,'base64') FROM jsonb_array_elements(COALESCE($1::jsonb->'chunks','[]'::jsonb)) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
	n = SPI_processed;
	if (!n)
		return;
	hashes = palloc((Size) n * PL_HASH_SIZE);
	data = palloc(sizeof(uint8 *) * n);
	lengths = palloc(sizeof(Size) * n);
	found = palloc0(sizeof(bool) * n);
	for (int i = 0; i < n; i++)
	{
		bytea	   *hash = DatumGetByteaPCopy(cell(i, 1));
		bytea	   *bytes = DatumGetByteaPCopy(cell(i, 2));
		PLSlice		slice = {(uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes)};
		PLHash		actual;

		if (VARSIZE_ANY_EXHDR(hash) != PL_HASH_SIZE || !pl_node_address(slice, &actual) ||
			memcmp(actual.bytes, VARDATA_ANY(hash), PL_HASH_SIZE))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("backup chunk hash mismatch")));
		memcpy(hashes + (Size) i * PL_HASH_SIZE, actual.bytes, PL_HASH_SIZE);
		data[i] = slice.data;
		lengths[i] = slice.len;
	}
	chunks = dolt_chunk_open(DOLT_CHUNK_WRITE);
	dolt_chunk_exists_many(chunks, n, hashes, found);
	for (int i = 0; i < n; i++)
	{
		if (found[i])
			continue;
		memmove(hashes + (Size) missing * PL_HASH_SIZE, hashes + (Size) i * PL_HASH_SIZE, PL_HASH_SIZE);
		data[missing] = data[i];
		lengths[missing++] = lengths[i];
	}
	if (missing)
		dolt_chunk_put_many(chunks, missing, hashes, data, lengths);
	dolt_chunk_close(chunks);
}

static char *
bundle_path(const char *url)
{
	char *path;

	if (strncmp(url, "file:///", 8))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("only absolute file:// repository URLs are supported")));
	path = psprintf("%s/manifest.bundle", url + 7);
	if (strlen(path) >= MAXPGPATH)
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("backup path is too long")));
	return path;
}

static char *
bundle_capture(void)
{
	StringInfoData refs;
	StringInfoData sequences;
	char **names;
	bytea **targets;
	char *objects;
	char *extensions;
	char *chunks;
	char **definitions;
	int definition_count;
	int count;

	ensure_repository();
	run("SELECT refname,reftarget FROM pg_dolt_ref ORDER BY refname COLLATE \"C\"", 0, NULL, NULL, NULL, true);
	count = SPI_processed;
	names = palloc(sizeof(char *) * count);
	targets = palloc(sizeof(bytea *) * count);
	for (int i = 0; i < count; i++)
	{
		names[i] = strcell(i, 1);
		targets[i] = DatumGetByteaPCopy(cell(i, 2));
	}

	initStringInfo(&refs);
	appendStringInfoChar(&refs, '[');
	for (int i = 0; i < count; i++)
	{
		if (!strncmp(names[i], "working/", 8))
		{
			const char *previous = dg_switch_execution_branch(names[i] + 8);

			PG_TRY();
			{
				targets[i] = capture(true).hash;
			}
			PG_FINALLY();
			{
				dg_switch_execution_branch(previous);
			}
			PG_END_TRY();
		}
		if (i)
			appendStringInfoChar(&refs, ',');
		appendStringInfoChar(&refs, '[');
		escape_json(&refs, names[i]);
		appendStringInfoChar(&refs, ',');
		escape_json(&refs, hash_format(targets[i]));
		appendStringInfoChar(&refs, ']');
	}
	appendStringInfoChar(&refs, ']');
	advance_snapshot();
	/* Extension installation is native cluster infrastructure, not part of a
	 * versioned schema. Reinstall the exact extension versions at the target. */
	run("SELECT COALESCE(jsonb_agg(jsonb_build_array(e.extname,e.extversion,n.nspname) ORDER BY e.extname),'[]'::jsonb)::text FROM pg_extension e JOIN pg_namespace n ON n.oid=e.extnamespace WHERE e.extname <> 'plpgsql'", 0, NULL, NULL, NULL, true);
	extensions = strcell(0, 1);
	/* Include retained historical objects as well as the live working sets.
	 * JSON is a framing format only; payload bytes retain their native codec. */
	run("SELECT COALESCE(jsonb_agg(jsonb_build_array(dolt_hash_encode(objhash),objkind,encode(objpayload,'base64'),ARRAY(SELECT dolt_hash_encode(h) FROM unnest(objchildren) h))),'[]'::jsonb)::text FROM pg_dolt_object_visible", 0, NULL, NULL, NULL, true);
	objects = strcell(0, 1);
	{
		bytea	  **roots = palloc(sizeof(bytea *) * Max(count, 1));
		int			nroots = 0;

		for (int i = 0; i < count; i++)
			if (!strncmp(names[i], "working/", 8) || !strncmp(names[i], "staged/", 7))
				roots[nroots++] = targets[i];
		bundle_sequences = NIL;
		chunks = bundle_chunks(roots, nroots);
	}
	definition_count = list_length(bundle_sequences);
	definitions = palloc(sizeof(char *) * Max(definition_count, 1));
	foreach_ptr(bytea, schema, bundle_sequences)
		definitions[foreach_current_index(schema)] = metadata(schema, "schema");
	bundle_sequences = NIL;
	initStringInfo(&sequences);
	appendStringInfoChar(&sequences, '[');
	for (int i = 0; i < definition_count; i++)
	{
		if (!OidIsValid(sequence_resource(definitions[i], false)))
			continue;
		if (sequences.len > 1)
			appendStringInfoChar(&sequences, ',');
		appendStringInfoString(&sequences, sequence_capture(definitions[i]));
	}
	appendStringInfoChar(&sequences, ']');
	return psprintf("{\"format\":\"DGBUNDLE1\",\"encoding\":%d,\"refs\":%s,\"objects\":%s,\"chunks\":%s,\"sequences\":%s,\"extensions\":%s}", GetDatabaseEncoding(), refs.data, objects, chunks, sequences.data, extensions);
}

static void
bundle_write(const char *url)
{
	char *path = bundle_path(url);
	char *directory = pstrdup(url + 7);
	char *temporary = psprintf("%s.%d.tmp", path, MyProcPid);
	char *payload = bundle_capture();
	PLHash hash;
	PLSlice bytes = {(const uint8 *) payload, strlen(payload)};
	FILE *volatile file;

	if (!pl_hash_bytes(bytes, &hash))
		elog(ERROR, "bundle checksum failed");
	if (pg_mkdir_p(directory, pg_dir_create_mode) < 0 && errno != EEXIST)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create backup directory \"%s\": %m", directory)));
	file = AllocateFile(temporary, PG_BINARY_W);
	if (!file)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create backup file: %m")));
	PG_TRY();
	{
		if (fwrite(DG_BUNDLE_MAGIC, 1, strlen(DG_BUNDLE_MAGIC), file) != strlen(DG_BUNDLE_MAGIC) ||
			fwrite(hash.bytes, 1, PL_HASH_SIZE, file) != PL_HASH_SIZE ||
			fwrite(payload, 1, bytes.len, file) != bytes.len || fflush(file) || pg_fsync(fileno(file)))
			ereport(ERROR, (errcode_for_file_access(), errmsg("could not write backup file: %m")));
		{
			int status = FreeFile(file);

			file = NULL;
			if (status)
				ereport(ERROR, (errcode_for_file_access(), errmsg("could not close backup file: %m")));
		}
		durable_rename(temporary, path, ERROR);
	}
	PG_CATCH();
	{
		if (file)
			FreeFile(file);
		unlink(temporary);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static char *
bundle_read(const char *url)
{
	char *path = bundle_path(url);
	FILE *file = AllocateFile(path, PG_BINARY_R);
	struct stat st;
	Size header = strlen(DG_BUNDLE_MAGIC) + PL_HASH_SIZE;
	char *data;
	PLHash hash;
	PLSlice bytes;

	if (!file)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not open backup \"%s\": %m", path)));
	if (fstat(fileno(file), &st) || st.st_size <= header || st.st_size >= MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid backup bundle size")));
	data = palloc(st.st_size + 1);
	if (fread(data, 1, st.st_size, file) != st.st_size)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not read backup: %m")));
	FreeFile(file);
	data[st.st_size] = '\0';
	bytes.data = (const uint8 *) data + header;
	bytes.len = st.st_size - header;
	if (memcmp(data, DG_BUNDLE_MAGIC, strlen(DG_BUNDLE_MAGIC)) ||
		!pl_hash_bytes(bytes, &hash) || memcmp(hash.bytes, data + strlen(DG_BUNDLE_MAGIC), PL_HASH_SIZE))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid backup bundle checksum or format")));
	return data + header;
}

static void
bundle_import_object(HTAB *objects, BundleObject *object)
{
	Datum *children;
	bool *nulls;
	int count;
	bytea *actual;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (object->state == 2)
		return;
	if (object->state == 1)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("cycle in backup object graph")));
	object->state = 1;
	deconstruct_array(object->children, BYTEAOID, -1, false, TYPALIGN_INT, &children, &nulls, &count);
	for (int i = 0; i < count; i++)
	{
		BundleObject *child;
		char *key;

		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("null backup child")));
		if (dg_manifest_tree_edge(object->kind, i) || dg_manifest_root_edge(object->kind, i))
			continue;			/* a tree or database root, put with the chunks */
		key = hash_format(DatumGetByteaPP(children[i]));
		child = hash_search(objects, key, HASH_FIND, NULL);
		if (!child)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("missing backup child %s", key)));
		bundle_import_object(objects, child);
	}
	actual = dg_manifest_put(object->kind, object->payload, object->children);
	if (strcmp(hash_format(actual), object->key))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("backup object hash mismatch")));
	object->state = 2;
}

static void
bundle_import(const char *url)
{
	char *payload = bundle_read(url);
	HASHCTL control = {0};
	HTAB *objects;
	HASH_SEQ_STATUS scan;
	BundleObject *object;
	char **names;
	bytea **targets;
	int count;

	run("SELECT $1::jsonb->>'format',($1::jsonb->>'encoding')::integer", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
	if (strcmp(strcell(0, 1), "DGBUNDLE1") || DatumGetInt32(cell(0, 2)) != GetDatabaseEncoding())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("incompatible backup format or database encoding")));
	control.keysize = sizeof(object->key);
	control.entrysize = sizeof(BundleObject);
	objects = hash_create("backup objects", 1024, &control, HASH_ELEM | HASH_STRINGS);
	run("SELECT v->>0,v->>1,decode(v->>2,'base64'),ARRAY(SELECT dolt_hash_decode(h) FROM jsonb_array_elements_text(v->3) h) FROM jsonb_array_elements($1::jsonb->'objects') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
	for (int i = 0; i < SPI_processed; i++)
	{
		char *key = strcell(i, 1);
		bool found;

		hash_parse(key);
		object = hash_search(objects, key, HASH_ENTER, &found);
		if (found)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("duplicate backup object")));
		object->kind = strcell(i, 2);
		object->payload = DatumGetByteaPCopy(cell(i, 3));
		object->children = DatumGetArrayTypePCopy(cell(i, 4));
		object->state = 0;
	}
	bundle_import_chunks(payload);
	hash_seq_init(&scan, objects);
	while ((object = hash_seq_search(&scan)) != NULL)
		bundle_import_object(objects, object);
	advance_snapshot();
	{
		char **names;
		char **versions;
		char **schemas;
		int n;

		run("SELECT v->>0,v->>1,v->>2 FROM jsonb_array_elements($1::jsonb->'extensions') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
		n = SPI_processed;
		names = palloc(sizeof(char *) * n);
		versions = palloc(sizeof(char *) * n);
		schemas = palloc(sizeof(char *) * n);
		for (int i = 0; i < n; i++)
		{
			names[i] = strcell(i, 1);
			versions[i] = strcell(i, 2);
			schemas[i] = strcell(i, 3);
		}
		for (int i = 0; i < n; i++)
		{
			run(psprintf("CREATE SCHEMA IF NOT EXISTS %s", quote_identifier(schemas[i])), 0, NULL, NULL, NULL, false);
			run(psprintf("CREATE EXTENSION %s WITH SCHEMA %s VERSION %s CASCADE", quote_identifier(names[i]), quote_identifier(schemas[i]), quote_literal_cstr(versions[i])), 0, NULL, NULL, NULL, false);
		}
	}
	/* Allocation state is shared across branches and also survives when its
	 * definition exists only in retained history. Install it before bindings. */
	{
		char **definitions;
		int n;

		run("SELECT v::text FROM jsonb_array_elements($1::jsonb->'sequences') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
		n = SPI_processed;
		definitions = palloc(sizeof(char *) * n);
		for (int i = 0; i < n; i++)
			definitions[i] = strcell(i, 1);
		for (int i = 0; i < n; i++)
			sequence_state(definitions[i], true, false);
	}
	run("SELECT v->>0,dolt_hash_decode(v->>1) FROM jsonb_array_elements($1::jsonb->'refs') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(payload)), NULL, true);
	count = SPI_processed;
	names = palloc(sizeof(char *) * count);
	targets = palloc(sizeof(bytea *) * count);
	for (int i = 0; i < count; i++)
	{
		names[i] = strcell(i, 1);
		targets[i] = DatumGetByteaPCopy(cell(i, 2));
	}
	for (int i = 0; i < count; i++)
		move_ref(names[i], read_ref(names[i]), targets[i]);
	advance_snapshot();
	for (int i = 0; i < count; i++)
	{
		if (!strncmp(names[i], "working/", 8))
		{
			int members_count;
			Member *members = database_members(targets[i], &members_count);

			replace_working(names[i] + 8, targets[i], NIL);
			for (int m = 0; m < members_count; m++)
				if (!strcmp(members[m].kind, "sequence"))
					sequence_state(metadata(child(members[m].hash, 1), "schema"), true, false);
		}
	}
	hash_destroy(objects);
}

/* A backend cannot change its connected database. Restore uses short-lived
 * internal workers: create a hidden staging database, import transactionally,
 * then publish its name. No connection can observe half-installed bindings. */
typedef struct BundleRequest
{
	int operation;
	Oid user;
	Oid staging;
	bool force;
	bool success;
	int sqlstate;
	char name[NAMEDATALEN];
	char temporary[NAMEDATALEN];
	char previous[NAMEDATALEN];
	char url[MAXPGPATH];
	char message[1024];
} BundleRequest;

void
DoltBundleWorkerMain(Datum argument)
{
	dsm_segment *segment;
	BundleRequest *request;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	segment = dsm_attach(DatumGetUInt32(argument));
	if (!segment)
		proc_exit(1);
	request = dsm_segment_address(segment);
	BackgroundWorkerInitializeConnectionByOid(request->operation == 1 ? request->staging : Template1DbOid,
		request->user, BGWORKER_BYPASS_ALLOWCONN);
	PG_TRY();
	{
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		if (request->operation == 0)
		{
			CreatedbStmt *statement = makeNode(CreatedbStmt);

			statement->dbname = request->temporary;
			statement->options = list_make2(
				makeDefElem("template", (Node *) makeString("template0"), -1),
				makeDefElem("allow_connections", (Node *) makeBoolean(false), -1));
			request->staging = createdb(NULL, statement);
		}
		else if (request->operation == 1)
		{
			int nest = begin_api();

			bundle_import(request->url);
			end_api(nest);
		}
		else if (request->operation == 2)
		{
			int nest = begin_api();

			if (OidIsValid(get_database_oid(request->name, true)))
			{
				if (!request->force)
					ereport(ERROR, (errcode(ERRCODE_DUPLICATE_DATABASE), errmsg("database '%s' already exists, use '--force' to overwrite", request->name)));
				RenameDatabase(request->name, request->previous);
				CommandCounterIncrement();
			}
			RenameDatabase(request->temporary, request->name);
			CommandCounterIncrement();
			run(psprintf("ALTER DATABASE %s ALLOW_CONNECTIONS true", quote_identifier(request->name)), 0, NULL, NULL, NULL, false);
			end_api(nest);
		}
		else
			dropdb(request->operation == 3 ? request->temporary : request->previous, true, false);
		PopActiveSnapshot();
		CommitTransactionCommand();
		request->success = true;
	}
	PG_CATCH();
	{
		ErrorData *error;

		MemoryContextSwitchTo(TopMemoryContext);
		error = CopyErrorData();
		strlcpy(request->message, error->message, sizeof(request->message));
		request->sqlstate = error->sqlerrcode;
		FlushErrorState();
		AbortCurrentTransaction();
	}
	PG_END_TRY();
	dsm_detach(segment);
	proc_exit(0);
}

static void
bundle_worker(BundleRequest *request)
{
	dsm_segment *segment = dsm_create(sizeof(BundleRequest), 0);
	BundleRequest *shared = dsm_segment_address(segment);
	BackgroundWorker worker = {0};
	BackgroundWorkerHandle *handle;

	memcpy(shared, request, sizeof(*shared));
	shared->success = false;
	shared->message[0] = '\0';
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_notify_pid = MyProcPid;
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(segment));
	strlcpy(worker.bgw_name, "Dolt backup restore", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "Dolt backup restore", BGW_MAXLEN);
	strlcpy(worker.bgw_library_name, "postgres", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "DoltBundleWorkerMain", BGW_MAXLEN);
	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED), errmsg("no background worker slot available for restore")));
	PG_TRY();
	{
		if (WaitForBackgroundWorkerShutdown(handle) != BGWH_STOPPED)
			elog(ERROR, "backup worker did not stop cleanly");
	}
	PG_CATCH();
	{
		/* Stop before the caller attempts to remove its staging database. */
		HOLD_INTERRUPTS();
		TerminateBackgroundWorker(handle);
		WaitForBackgroundWorkerShutdown(handle);
		RESUME_INTERRUPTS();
		PG_RE_THROW();
	}
	PG_END_TRY();
	memcpy(request, shared, sizeof(*request));
	dsm_detach(segment);
	if (!request->success)
		ereport(ERROR, (errcode(request->sqlstate ? request->sqlstate : ERRCODE_INTERNAL_ERROR),
						errmsg("backup restore failed: %s", request->message[0] ? request->message : "worker exited unexpectedly")));
}

static void
bundle_restore(const char *url, const char *name, bool force)
{
	BundleRequest request = {0};
	bool published = false;

	bundle_path(url);
	if (!name[0] || strlen(name) >= NAMEDATALEN)
		ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("invalid restore database name")));
	if (get_database_oid(name, true) == MyDatabaseId)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("cannot restore over the connected database")));
	if (!force && OidIsValid(get_database_oid(name, true)))
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_DATABASE), errmsg("database '%s' already exists, use '--force' to overwrite", name)));
	request.user = GetUserId();
	request.force = force;
	strlcpy(request.name, name, sizeof(request.name));
	strlcpy(request.url, url, sizeof(request.url));
	snprintf(request.temporary, sizeof(request.temporary), "dolt_restore_%d_" INT64_FORMAT, MyProcPid, GetCurrentTimestamp());
	snprintf(request.previous, sizeof(request.previous), "dolt_replaced_%d_" INT64_FORMAT, MyProcPid, GetCurrentTimestamp());
	PG_TRY();
	{
		bundle_worker(&request);
		request.operation = 1;
		bundle_worker(&request);
		request.operation = 2;
		bundle_worker(&request);
		published = true;
	}
	PG_CATCH();
	{
		MemoryContext context = MemoryContextSwitchTo(TopMemoryContext);
		ErrorData *original = CopyErrorData();

		FlushErrorState();
		PG_TRY(cleanup);
		{
			request.operation = 3;
			bundle_worker(&request);
		}
		PG_CATCH(cleanup);
		{
			FlushErrorState();
		}
		PG_END_TRY(cleanup);
		MemoryContextSwitchTo(context);
		ReThrowError(original);
	}
	PG_END_TRY();
	if (published && force)
	{
		/* Cleanup is a separate commit: dropping old files must never precede
		 * the commit that publishes the new database name. */
		request.operation = 4;
		bundle_worker(&request);
	}
}
