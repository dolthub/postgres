# Built-in prolly table and index access methods.
subdir = src/backend/access/dolt_prolly
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dg_branch.o dg_chunk.o dg_chunk_heap.o dg_system.o dg_version.o dg_manifest.o dg_node_cache.o dg_schema_cache.o dg_store.o dg_lock.o dg_identity.o dg_statistics.o dg_resource.o dg_table.o dg_catalog_scan.o dg_codec.o dg_index.o pl_store.o pl_tree.o pl_tuple.o pl_version.o pl_file.o
$(OBJS): dg.h $(top_srcdir)/src/doltgres/prolly/prolly.h $(top_srcdir)/src/doltgres/prolly/internal.h

include $(top_srcdir)/src/backend/common.mk

# dg_version.c includes the command slices and generated document text.
dg_version.o: $(wildcard $(srcdir)/*.inc) $(srcdir)/dg_agent_doc.h
