# Database root plan

Status, 2026-11-05, branch `zachmu/commit-tree-roots`: milestones 1 and 2
are done (`1f0622b2af1`, `d89153b6376`); milestone 3 is open. This followed
commits that stopped copying trees ([chunk store plan](CHUNK-STORE-PLAN.md)
milestone 6) and that install captured trees as bases
([delta store plan](DELTA-STORE-PLAN.md) §7).

As built, beyond the design below:

- **Extensions are stored,** as tagged field 2 of the top node: the
  extensions a state's definitions use, computed when the root is written.
  Deriving them on read consults the server's current extensions, so a
  history that used a since-dropped extension would no longer say so.
- **Sequences fingerprint without their seed.** A sequence's schema object
  keeps its allocation seed for restore, so its address changes with
  allocation. The rendering's `schema` field for a sequence is the address
  of its definition without the seed, and branch-dirty checks compare
  renderings, not root addresses, so allocation alone changes nothing.
- **Roots need no visibility probe.** A root's address comes only from a
  visible ref or object, and its bytes are fixed by its hash, so readers
  read it through the shared node cache. A transaction remembers the roots
  it wrote, with the writing subtransaction's xid, for the catalog
  provider's creator checks.
- **`dolt_schema_address(definition)`** gives the address a schema
  definition would have without writing it; fingerprints computed in SQL use
  it.
- Bundles carry root nodes and the trees they name, and collect sequence
  definitions from roots.

Measured on a release build (folds off), against the previous commit:

| | 10 tables | 100 tables | 1,000 tables |
|---|---|---|---|
| CREATE TABLE | 14.5 → 13.5 ms | 36 → 35 ms | 307 → 231 ms |
| ALTER TABLE | 4.4 → 4.0 ms | 20 → 18 ms | 224 → 168 ms |
| one-row commit | 0.95 → 0.85 ms | 1.5 → 2.1 ms | 8.4 → 8.0 ms |
| `dolt_status` | 1.6 → 1.6 ms | 7.0 → 7.8 ms | 75 → 74 ms |
| new session, one SELECT | 5.2 → 5.1 ms | 11 → 11 ms | 82 → 71 ms |

Reading and writing roots no longer shows in profiles. At 1,000 tables, a
CREATE TABLE spends 58% rebuilding the catalog view of the whole schema
(`provider_populate`), 14% capturing, 10% moving the working ref and
syncing identities, and 5% building the root, most of it computing
extensions. The remaining costs grow with the schema because consumers
still walk every member:

- **The catalog provider rebuilds a view from every definition** for each
  new root. Two roots can now be diffed by walking their nodes, so a view
  could be derived from the previous one plus the changed members, and
  hydrated definitions shared by schema address.
- **Version-control commands read a DGDB1 rendering** (milestone 3).
- **Captures still visit every member.**

## Problem

A database state is a `database` object in `pg_dolt_object`:

- a DGDB1 JSON payload listing every member (kind, name, a hex SHA-256
  "schema" fingerprint, a content root), namespace definitions inline, and a
  derived `dependents` map;
- children: one member object per table, index, view, routine, schema and so
  on, each another JSON payload repeating kind, name, fingerprint and content,
  with the schema object and content root as its children.

Any DDL or capture rewrites every member's entry and re-sorts the list in SQL.
Reading one member parses JSON or walks the member objects into a backend
cache. Diff, status and merge compare whole lists.

## Design

A database state is a **root**: one chunk in the chunk store, named by its
hash like any tree node, read with zero parsing, with each member found by
binary search. It spills to more chunks only when it outgrows a page.

Decisions (2026-11-05):

- **Our own binary format,** not flatbuffers: smaller, canonical by
  construction (a chunk's hash must follow from its contents alone), and no
  vendored code generator. Dolt interop is a non-goal.
- **Members are keyed by kind and name,** for history independence: the
  same contents give the same root, however they came about.
- **Everything a database holds is in the root:** every table, index,
  namespace, view, routine, sequence, trigger, type and the rest, inline as
  entries. No member objects.
- **Schemas by address.** An entry names its schema object; schema
  definitions are immutable and cached hydrated.
- **Dependents are derived,** from definitions and index owners, not stored.

### Chunk layout

Little-endian integers. A chunk is a **node**:

```
header (16 bytes)
  0  magic "DGR1"
  4  u8  level        0 = leaf, n = index over level n-1 nodes
  5  u8  reserved
  6  u16 stride       bytes per slot; readers skip slot bytes they don't know
  8  u32 count        slots
  12 u32 extension    offset of the node's tagged fields, 0 if none
slots[count]          fixed width, sorted by key
heap                  keys and tagged fields, referenced by offset
```

A **leaf slot** (stride 52) is one member:

```
  0  u32 key offset   key = kind, NUL, qualified name
  4  u16 key length
  6  u16 flags        bit 0: has content
  8  [20] schema      the schema object's address
  28 [20] content     the content tree's root, zeros without the flag
  48 u32 extension    offset of the entry's tagged fields, 0 if none
```

An **index slot** (stride 32) is one child:

```
  0  u32 key offset   the child's last key
  4  u16 key length
  6  u16 reserved
  8  [20] child       the child node's address
  28 u32 count        members under the child
```

**Tagged fields** are `u8 tag, u16 length, bytes`, ascending by tag, ending
with tag 0. Old readers skip unknown tags. The top node's fields hold
database-wide values: tag 1 is the server encoding name.

Keys compare as unsigned bytes. Kind first means relations, routines and
types sort in groups; a routine's name carries its argument signature.

### Spilling

- If one leaf of all members fits the page budget (8,000 bytes), the root is
  that leaf.
- Otherwise members are cut into leaves, and those into index nodes, until
  one node holds the rest. A leaf ends after a member whose key hash is
  0 mod 32, once it holds 2,000 bytes, or before it would pass the budget.
  Boundaries follow from the keys alone, so one change rewrites one leaf and
  its path.
- Builds always start from the full sorted member list, which is cheap for
  thousands of members; only chunks the store lacks are written.
- A lookup is a binary search per level, through a backend cache of node
  bytes keyed by hash.

### Members as values

Code that handled member object hashes handles **member references**: the
entry encoded on its own (version, flags, key, schema, content). Equal
references are equal members. `child()` and `metadata()` decode them without
a catalog read. Building a root from references needs no other lookup.

### Edges

The commit's first child, a merge's working and staged states and a stash's
states are root edges: `put_object` checks the chunk store for them, and
bundles carry root nodes and the content trees they name. `working/` and
`staged/` refs target roots. The `database` kind and the member kinds
(`table`, `index`, `routine`, `trigger`, `type`, `view`, `sequence`,
`aggregate`, `operator`, `cast`) are retired; `namespace` and `schema`
objects remain as the definitions entries name.

## Milestones

1. **Format library** (`dg_root.c`): build, spill, lookup, iterate, node
   cache, member references. *Gate:* unit tests through a test function:
   round trips, binary search, spill at the budget, boundary stability under
   one insert.
2. **Switch the state:** roots for `database` objects, references for member
   objects, root edges in commits, merges, stashes, refs and bundles; SQL that
   joined member objects reads `dolt_root_members()` instead. JSON consumers
   in diff, status and merge read a DGDB1-shaped rendering of the root
   (`dolt_root_json()`), cached by hash. *Gate:* test_prolly, test_doltgres
   and test_dolt_chunk as before.
3. **Retire the rendering:** diff, status, merge and DDL read members through
   the C interface. Removes the remaining JSON per state.

## Measurement

Commit and DDL latency with 10, 100 and 1,000 tables; bytes written per DDL;
member lookup cost.
