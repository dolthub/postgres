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
 * The C form of core's char properties and debug escape (the port text's
 * section 9).
 */
#ifndef DOLTGRES_STD_UNICODE_H
#define DOLTGRES_STD_UNICODE_H

extern bool std_char_is_control(uint32 c);
extern bool std_char_is_private_use(uint32 c);
extern bool std_char_is_whitespace(uint32 c);
extern bool std_char_is_grapheme_extender(uint32 c);
extern bool std_char_is_default_ignorable(uint32 c);
extern bool std_char_is_format_control(uint32 c);
extern bool std_char_is_assigned(uint32 c);
extern size_t std_char_escape_debug_ext(uint32 c, bool escape_grapheme_extender,
										bool escape_single_quote,
										bool escape_double_quote, char *buf,
										size_t *buf_len);

#endif							/* DOLTGRES_STD_UNICODE_H */
