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
 * The words of datums of types passed by value (pg's word.rs): each reader
 * and writer is the C of the macro of postgres.h its comment names, over the
 * 64 bits a Datum is on every platform.
 */
#ifndef DOLTGRES_PG_WORD_H
#define DOLTGRES_PG_WORD_H

#ifndef FRONTEND

extern bool pg_word_to_bool(uint64 word);
extern uint8 pg_word_to_char(uint64 word);
extern int16 pg_word_to_int16(uint64 word);
extern int32 pg_word_to_int32(uint64 word);
extern int64 pg_word_to_int64(uint64 word);
extern uint32 pg_word_to_oid(uint64 word);
extern float4 pg_word_to_float4(uint64 word);
extern float8 pg_word_to_float8(uint64 word);
extern uint64 pg_word_from_bool(bool value);
extern uint64 pg_word_from_char(uint8 value);
extern uint64 pg_word_from_int16(int16 value);
extern uint64 pg_word_from_int32(int32 value);
extern uint64 pg_word_from_int64(int64 value);
extern uint64 pg_word_from_oid(uint32 value);

#ifdef DOLTGRES_TEST_PG
extern void test_pg_word_tests_words_read_back_as_they_were_written(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_PG_WORD_H */
