/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The delta's doors over the fork's region (pghost's delta.rs, design item
 * 14's S6): its page directory, its blocks, and its aged records.
 */
#ifndef DOLTGRES_PGHOST_DELTA_H
#define DOLTGRES_PGHOST_DELTA_H

#ifndef FRONTEND

#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/delta/directory.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"

/* PghostDeltaRecoveryFold is the server's fold in recovery, which a wait at the delta's budget runs in a startup process. */
typedef bool (*PghostDeltaRecoveryFold) (HostIoIo *io, bool *result, HostError *error);

extern size_t pghost_delta_bytes(void);
extern size_t pghost_delta_kind_of(uint32 aged);
extern bool pghost_delta_make(const HostSharedKind *kind, const char *name, size_t name_len, bool *result_ok,
							  uint64 *result_0, uint64 *result_1, uint64 *result_err, HostError *error);
extern bool pghost_delta_record(const HostSharedKind *kind, uint64 location, uint64 epoch, uint64 *result,
								HostError *error);
extern void pghost_delta_publish(uint64 place);
extern void pghost_delta_unmake(uint64 place);
extern bool pghost_delta_hold(const HostMayRaise *raise, uint64 *result, HostError *error);
extern void pghost_delta_release(uint64 epoch);
extern void pghost_delta_ended(uint64 epoch);
extern bool pghost_delta_open(HostDeltaPagePage name, uint64 epoch, HostDeltaDirectoryOpened *result,
							  HostError *error);
extern bool pghost_delta_bytes_of(const HostDeltaDirectoryOpened *opened, uint64 offset, uint64 len,
								  HostCacheBytes *result, HostError *error);
extern uint64 pghost_delta_room(const HostDeltaDirectoryOpened *opened);
extern uint64 pghost_delta_filled(const HostDeltaDirectoryOpened *opened);
extern uint64 pghost_delta_word(uint64 body, uint64 index, HostDeltaWordOp op);
extern bool pghost_delta_alloc(const HostMayRaise *raise, const HostDeltaPageSize *sizes, size_t sizes_len,
							   StdVec *names, bool *has_result, uint64 *result, HostError *error);
extern bool pghost_delta_alloc_replacing(const HostMayRaise *raise, HostDeltaPageSize size,
										 HostDeltaPagePage replaced, StdVec *names, bool *has_result,
										 uint64 *result, HostError *error);
extern void pghost_delta_give_back(HostDeltaPagePage name);
extern bool pghost_delta_owned_append(HostDeltaPagePage name, const uint8 *record, size_t record_len,
									  uint32 *result, HostError *error);
extern bool pghost_delta_owned_store(HostDeltaPagePage name, uint64 index, uint64 value, HostError *error);
extern void pghost_delta_link(HostDeltaPagePage name, HostDeltaDirectoryOwner owner);
extern bool pghost_delta_append(const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
								const uint8 *record, size_t record_len, uint32 *result, HostError *error);
extern bool pghost_delta_seal(const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
							  HostError *error);
extern void pghost_delta_retire(const HostDeltaLeaving *records, size_t records_len,
								const HostDeltaPagePage *names, size_t names_len);
extern bool pghost_delta_packing(const uint64 *shapes, size_t shapes_len, StdVec *result);
extern void pghost_delta_pass(void);
extern uint64 pghost_delta_used(void);
extern void pghost_delta_recovery_fold(PghostDeltaRecoveryFold fold);
extern bool pghost_delta_wait(const HostMayRaise *raise, uint64 frees, HostError *error);
extern void pghost_delta_exit(void);

#endif							/* FRONTEND */

#endif							/* DOLTGRES_PGHOST_DELTA_H */
