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
 * The free space of a directory's volume, which the collector's schedule reads.
 */
#ifndef DOLTGRES_DISK_SPACE_H
#define DOLTGRES_DISK_SPACE_H

/*
 * doltgres_disk_space stores the bytes a directory's volume holds and those of
 * them still free to this process.
 */
extern void doltgres_disk_space(const char *path, uint64 *available, uint64 *size);

#endif							/* DOLTGRES_DISK_SPACE_H */
