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
 * The C port of txn's memtable/pins.rs: a memtable's pins, a word of each
 * process's slot for each kind of memtable, naming the memtable one past its
 * location from a prepare's hand-off to its settle, so that no close takes
 * the memtable's first range away from a finish; a process's exit zeroes its
 * words.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/shared.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/pins.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "pins.rs"

/* TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS is Pins' ProcSlots: each process's slot of pins, a word for each kind of memtable. */
const HostSharedProcSlotsOps TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS = {
	.id = UINT64CONST(0x74786e000012),
	.words = 5,
};

/* txn_memtable_pins_pinned_frame_0_t is pinned's state across its pad: the processes' list, while live. */
typedef struct txn_memtable_pins_pinned_frame_0_t
{
	size_t		word;
	uint64		memtable;
	bool	   *result;
	HostError  *error;
	bool		ok;
	StdVec		processes;
	bool		processes_live;
} txn_memtable_pins_pinned_frame_0_t;

/* txn_memtable_pins_pinned_pad_0 is pinned's pad: the processes' list, while live. */
static void
txn_memtable_pins_pinned_pad_0(void *frame_)
{
	txn_memtable_pins_pinned_frame_0_t *frame = frame_;

	if (frame->processes_live)
	{
		frame->processes_live = false;
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	}
}

/* txn_memtable_pins_pinned_body_0 is pinned's body. */
static void
txn_memtable_pins_pinned_body_0(void *frame_)
{
	txn_memtable_pins_pinned_frame_0_t *frame = frame_;

	*frame->result = false;
	if (!host_shared_processes(&TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS, &frame->processes, frame->error))
		return;
	frame->processes_live = true;
	for (size_t k = 0, k_len = frame->processes.len; k < k_len; k++)
	{
		uint64		pinned;

		if (!host_shared_load(&TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS, frame->word,
							  STD_VEC_AT(&frame->processes, uint32, k), &pinned, frame->error))
		{
			txn_memtable_pins_pinned_pad_0(frame);
			return;
		}
		if (pinned == STD_ADD(u64, frame->memtable, 1, 69, 49))
		{
			*frame->result = true;
			break;
		}
	}
	frame->ok = true;
	txn_memtable_pins_pinned_pad_0(frame);
}

/*
 * txn_memtable_pins_pinned reports whether any process's pin of a kind names
 * a memtable, read word by word, for a closer that holds the lock a pin's
 * prepare checks after its store.
 */
bool
txn_memtable_pins_pinned(size_t word, uint64 memtable, bool *result, HostError *error)
{
	txn_memtable_pins_pinned_frame_0_t frame = {.word = word, .memtable = memtable, .result = result, .error = error};

	std_landing(&frame, txn_memtable_pins_pinned_body_0, txn_memtable_pins_pinned_pad_0);
	return frame.ok;
}

/* txn_memtable_pins_pin stores the process's pin of a kind of a memtable. */
void
txn_memtable_pins_pin(const HostSharedMine *mine, size_t word, uint64 memtable)
{
	host_shared_Mine_store(mine, word, STD_ADD(u64, memtable, 1, 78, 21));
}

/* txn_memtable_pins_unpin clears the process's pin of a kind. */
void
txn_memtable_pins_unpin(const HostSharedMine *mine, size_t word)
{
	host_shared_Mine_store(mine, word, 0);
}
