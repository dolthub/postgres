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
 * A frontend program's host: host's reads over Postgres' port layer, through
 * pg's frontend wrappers, for a data directory a program was given, with
 * each error worded as Postgres' frontend programs word it. It gives the
 * reads alone, so nothing a frontend program calls can write.
 */
#ifndef DOLTGRES_FEHOST_LIB_H
#define DOLTGRES_FEHOST_LIB_H

#include "doltgres/host/io.h"
#include "doltgres/std/str.h"

extern bool fehost_install(const char *datadir, size_t datadir_len, HostIoReadIo *result, StdString *error);

#endif							/* DOLTGRES_FEHOST_LIB_H */
