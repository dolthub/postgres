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
 * section 9), ported from core/src/unicode/unicode_data.rs and
 * core/src/char/methods.rs at Rust 1.99.1, with core's tables, generated
 * from its source, and its search over them.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "port/pg_bitutils.h"

#include "doltgres/std/unicode.h"
#include "doltgres/std/vec.h"

/* STD_UNICODE_DATA_FILE is core's unicode/unicode_data.rs, where its lookups' checks report. */
#define STD_UNICODE_DATA_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "unicode" STD_SEP "unicode_data.rs"

/* STD_ESCAPE_FILE is core's escape.rs, where escape_unicode's checks report. */
#define STD_ESCAPE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "escape.rs"

/* RUN is a ShortOffsetRunHeader: its start index above 21 bits of prefix sum. */
#define RUN(start, sum) ((((uint32) (start)) << 21) | (uint32) (sum))
#define RUN_START(run) ((size_t) ((run) >> 21))
#define RUN_SUM(run) ((run) & ((1u << 21) - 1))

/* The tables of Unicode 17.0.0, as core 1.99.1 holds them. */

/* CF_RUNS is core's unicode_data::cf: its runs, as (start index, prefix sum). */
static const uint32 CF_RUNS[11] = {
	RUN(0, 1536), RUN(3, 2192), RUN(11, 6158), RUN(15, 8203), RUN(17, 65279), RUN(25, 69821), RUN(29, 78896), RUN(33, 113824), RUN(35, 119155), RUN(37, 917505), RUN(39, 2031744)
};

/* CF_OFFSETS is core's unicode_data::cf's offsets. */
static const uint8 CF_OFFSETS[43] = {
	173, 1, 0, 6, 22, 1, 192, 1, 49, 1, 0, 2, 80, 1, 0, 1,
	0, 5, 26, 5, 49, 5, 1, 10, 0, 1, 249, 3, 0, 1, 15, 1,
	0, 16, 0, 4, 0, 8, 0, 1, 30, 96, 0,
};

/* CF_THRESHOLD is the first char cf::lookup searches for. */
#define CF_THRESHOLD 0xad

/* CN_PLANES_0_3_RUNS is core's unicode_data::cn_planes_0_3: its runs, as (start index, prefix sum). */
static const uint32 CN_PLANES_0_3_RUNS[54] = {
	RUN(0, 888), RUN(1, 1328), RUN(11, 1806), RUN(25, 4681), RUN(325, 5789), RUN(365, 7958), RUN(445, 9258), RUN(491, 11124), RUN(495, 11508), RUN(497, 42125), RUN(549, 42540), RUN(553, 55204), RUN(605, 64110), RUN(611, 64976), RUN(629, 67383), RUN(735, 74650), RUN(1067, 77712), RUN(1074, 78934), RUN(1077, 82939), RUN(1079, 83527), RUN(1081, 90368), RUN(1082, 92160), RUN(1084, 92729), RUN(1085, 93504), RUN(1108, 101590), RUN(1127, 110576), RUN(1132, 110883), RUN(1139, 111356), RUN(1149, 113664), RUN(1150, 117760), RUN(1160, 118452), RUN(1163, 120486), RUN(1227, 120780), RUN(1229, 121484), RUN(1231, 122624), RUN(1236, 123536), RUN(1262, 124112), RUN(1268, 126065), RUN(1298, 126976), RUN(1370, 128729), RUN(1395, 129624), RUN(1423, 131072), RUN(1444, 173792), RUN(1445, 178206), RUN(1447, 183982), RUN(1449, 191457), RUN(1451, 192094), RUN(1453, 194560), RUN(1454, 195102), RUN(1455, 196608), RUN(1456, 201547), RUN(1457, 210042), RUN(1459, 262142), RUN(1460, 1376254)
};

/* CN_PLANES_0_3_OFFSETS is core's unicode_data::cn_planes_0_3's offsets. */
static const uint8 CN_PLANES_0_3_OFFSETS[1461] = {
	0, 2, 6, 4, 7, 1, 1, 1, 20, 1, 0, 1, 38, 2, 50, 2,
	3, 1, 55, 8, 27, 4, 6, 11, 0, 1, 60, 2, 101, 14, 59, 2,
	49, 2, 15, 1, 28, 2, 1, 1, 11, 5, 34, 5, 237, 1, 8, 2,
	2, 2, 22, 1, 7, 1, 1, 3, 4, 2, 9, 2, 2, 2, 4, 8,
	1, 4, 2, 1, 5, 2, 25, 2, 3, 1, 6, 4, 2, 2, 22, 1,
	7, 1, 2, 1, 2, 1, 2, 2, 1, 1, 5, 4, 2, 2, 3, 3,
	1, 7, 4, 1, 1, 7, 17, 10, 3, 1, 9, 1, 3, 1, 22, 1,
	7, 1, 2, 1, 5, 2, 10, 1, 3, 1, 3, 2, 1, 15, 4, 2,
	12, 7, 7, 1, 3, 1, 8, 2, 2, 2, 22, 1, 7, 1, 2, 1,
	5, 2, 9, 2, 2, 2, 3, 7, 3, 4, 2, 1, 5, 2, 18, 10,
	2, 1, 6, 3, 3, 1, 4, 3, 2, 1, 1, 1, 2, 3, 2, 3,
	3, 3, 12, 4, 5, 3, 3, 1, 4, 2, 1, 6, 1, 14, 21, 5,
	13, 1, 3, 1, 23, 1, 16, 2, 9, 1, 3, 1, 4, 7, 2, 1,
	3, 1, 2, 2, 4, 2, 10, 7, 22, 1, 3, 1, 23, 1, 10, 1,
	5, 2, 9, 1, 3, 1, 4, 7, 2, 5, 3, 1, 4, 2, 10, 1,
	3, 12, 13, 1, 3, 1, 51, 1, 3, 1, 6, 4, 16, 2, 26, 1,
	3, 1, 18, 3, 24, 1, 9, 1, 1, 2, 7, 3, 1, 4, 6, 1,
	1, 1, 8, 6, 10, 2, 3, 12, 58, 4, 29, 37, 2, 1, 1, 1,
	5, 1, 24, 1, 1, 1, 23, 2, 5, 1, 1, 1, 7, 1, 10, 2,
	4, 32, 72, 1, 36, 4, 39, 1, 36, 1, 15, 1, 13, 37, 198, 1,
	1, 5, 1, 2, 0, 1, 4, 2, 7, 1, 1, 1, 4, 2, 41, 1,
	4, 2, 33, 1, 4, 2, 7, 1, 1, 1, 4, 2, 15, 1, 57, 1,
	4, 2, 67, 2, 32, 3, 26, 6, 86, 2, 6, 2, 0, 3, 89, 7,
	22, 9, 24, 9, 20, 12, 13, 1, 3, 1, 2, 12, 94, 2, 10, 6,
	10, 6, 26, 6, 89, 7, 43, 5, 70, 10, 31, 1, 12, 4, 12, 4,
	1, 3, 42, 2, 5, 11, 44, 4, 26, 6, 11, 3, 62, 2, 65, 1,
	29, 2, 11, 6, 10, 6, 14, 2, 46, 2, 12, 20, 77, 1, 166, 8,
	60, 3, 15, 3, 62, 5, 43, 2, 11, 8, 43, 5, 0, 2, 6, 2,
	38, 2, 6, 2, 8, 1, 1, 1, 1, 1, 1, 1, 31, 2, 53, 1,
	15, 1, 14, 2, 6, 1, 19, 2, 3, 1, 9, 1, 101, 1, 12, 2,
	27, 1, 13, 3, 34, 14, 33, 15, 140, 4, 0, 22, 11, 21, 0, 2,
	0, 5, 45, 1, 1, 5, 1, 2, 56, 7, 2, 14, 24, 9, 7, 1,
	7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 7, 1, 126, 34,
	26, 1, 89, 12, 214, 26, 80, 1, 86, 2, 103, 5, 43, 1, 94, 1,
	86, 9, 48, 1, 0, 3, 55, 9, 0, 20, 184, 8, 221, 20, 60, 3,
	10, 6, 56, 8, 70, 8, 12, 6, 116, 11, 30, 3, 78, 1, 11, 4,
	33, 1, 55, 9, 14, 2, 10, 2, 103, 24, 28, 10, 6, 2, 6, 2,
	6, 9, 7, 1, 7, 1, 60, 4, 126, 2, 10, 6, 0, 12, 23, 4,
	49, 4, 0, 2, 106, 38, 7, 12, 5, 5, 26, 1, 5, 1, 1, 1,
	2, 1, 2, 1, 0, 32, 42, 6, 51, 1, 19, 1, 4, 4, 5, 1,
	135, 2, 1, 1, 190, 3, 6, 2, 6, 2, 6, 2, 3, 3, 7, 1,
	7, 10, 5, 2, 12, 1, 26, 1, 19, 1, 2, 1, 15, 2, 14, 34,
	123, 5, 3, 4, 45, 3, 88, 1, 13, 3, 1, 47, 46, 130, 29, 3,
	49, 15, 28, 4, 36, 9, 30, 5, 43, 5, 30, 1, 37, 4, 14, 42,
	158, 2, 10, 6, 36, 4, 36, 4, 40, 8, 52, 11, 12, 1, 15, 1,
	7, 1, 2, 1, 11, 1, 15, 1, 7, 1, 2, 3, 52, 12, 0, 9,
	22, 10, 8, 24, 6, 1, 42, 1, 9, 69, 6, 2, 1, 1, 44, 1,
	2, 3, 1, 2, 23, 1, 72, 8, 9, 48, 19, 1, 2, 5, 33, 3,
	27, 5, 27, 38, 56, 4, 20, 2, 50, 1, 2, 5, 8, 1, 3, 1,
	29, 2, 3, 4, 10, 7, 9, 7, 64, 32, 39, 4, 12, 9, 54, 3,
	29, 2, 27, 5, 26, 7, 4, 12, 7, 80, 73, 55, 51, 13, 51, 7,
	46, 8, 10, 6, 38, 3, 29, 8, 2, 208, 31, 1, 42, 1, 3, 2,
	2, 16, 6, 8, 9, 33, 46, 8, 42, 22, 26, 38, 28, 20, 23, 9,
	78, 4, 36, 9, 68, 10, 1, 2, 25, 7, 10, 6, 53, 1, 18, 8,
	39, 9, 96, 1, 20, 11, 18, 1, 47, 62, 7, 1, 1, 1, 4, 1,
	15, 1, 11, 6, 59, 5, 10, 6, 4, 1, 8, 2, 2, 2, 22, 1,
	7, 1, 2, 1, 5, 1, 10, 2, 2, 2, 3, 2, 1, 6, 1, 5,
	7, 2, 7, 3, 5, 11, 10, 1, 1, 2, 1, 1, 38, 1, 10, 1,
	1, 2, 1, 1, 4, 1, 10, 1, 2, 8, 2, 29, 92, 1, 5, 30,
	72, 8, 10, 166, 54, 2, 38, 34, 69, 11, 10, 6, 13, 19, 58, 6,
	10, 6, 20, 28, 27, 2, 15, 4, 23, 185, 60, 100, 83, 12, 8, 2,
	1, 2, 8, 1, 2, 1, 30, 1, 2, 2, 12, 9, 10, 70, 8, 2,
	46, 2, 11, 27, 72, 8, 83, 13, 73, 7, 10, 86, 8, 88, 34, 14,
	10, 6, 9, 1, 45, 1, 14, 10, 29, 3, 32, 2, 22, 1, 14, 73,
	7, 1, 2, 1, 44, 3, 1, 1, 2, 1, 9, 8, 10, 6, 6, 1,
	2, 1, 37, 1, 2, 1, 6, 7, 10, 6, 44, 4, 10, 246, 25, 7,
	17, 1, 41, 3, 29, 85, 1, 15, 50, 13, 0, 102, 111, 1, 5, 11,
	196, 0, 99, 13, 0, 10, 0, 5, 0, 0, 58, 0, 0, 7, 31, 1,
	10, 4, 81, 1, 10, 6, 30, 2, 6, 10, 70, 10, 10, 1, 7, 1,
	21, 5, 19, 0, 58, 198, 91, 5, 25, 2, 25, 44, 75, 4, 57, 7,
	17, 64, 5, 11, 7, 9, 0, 41, 32, 97, 115, 0, 4, 1, 7, 1,
	2, 1, 0, 15, 1, 29, 3, 2, 1, 14, 4, 8, 0, 0, 107, 5,
	13, 3, 9, 7, 10, 2, 8, 0, 253, 3, 0, 6, 23, 15, 17, 15,
	46, 2, 23, 9, 116, 60, 246, 10, 39, 2, 194, 21, 70, 122, 20, 12,
	20, 12, 87, 9, 25, 135, 85, 1, 71, 1, 2, 2, 1, 2, 2, 2,
	4, 1, 12, 1, 1, 1, 7, 1, 65, 1, 4, 2, 8, 1, 7, 1,
	28, 1, 4, 1, 5, 1, 1, 3, 7, 1, 0, 2, 0, 2, 0, 15,
	5, 1, 15, 0, 31, 6, 6, 213, 7, 1, 17, 2, 7, 1, 2, 1,
	5, 5, 62, 33, 1, 112, 45, 3, 14, 2, 10, 4, 2, 0, 31, 17,
	58, 5, 1, 0, 42, 214, 43, 4, 1, 192, 31, 1, 22, 8, 2, 224,
	7, 1, 4, 1, 2, 1, 15, 1, 197, 2, 16, 41, 76, 4, 10, 4,
	2, 0, 68, 76, 61, 194, 4, 1, 27, 1, 2, 1, 1, 2, 1, 1,
	10, 1, 4, 1, 1, 1, 1, 6, 1, 4, 1, 1, 1, 1, 1, 1,
	3, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	2, 1, 1, 2, 4, 1, 7, 1, 4, 1, 4, 1, 1, 1, 10, 1,
	17, 5, 3, 1, 5, 1, 17, 52, 2, 0, 44, 4, 100, 12, 15, 2,
	15, 1, 15, 1, 37, 10, 174, 56, 29, 13, 44, 4, 9, 7, 2, 14,
	6, 154, 0, 3, 17, 3, 13, 3, 218, 6, 12, 4, 1, 15, 12, 4,
	56, 8, 10, 6, 40, 8, 30, 2, 12, 4, 2, 14, 9, 39, 0, 8,
	14, 2, 13, 3, 11, 3, 57, 1, 1, 4, 16, 2, 12, 4, 10, 7,
	147, 1, 103, 0, 0, 32, 0, 2, 0, 2, 0, 15, 0, 0, 0, 0,
	0, 5, 0, 0, 0,
};

/* CN_PLANES_0_3_THRESHOLD is the first char cn_planes_0_3::lookup searches for. */
#define CN_PLANES_0_3_THRESHOLD 0x378

/* DEFAULT_IGNORABLE_CODE_POINT_RUNS is core's unicode_data::default_ignorable_code_point: its runs, as (start index, prefix sum). */
static const uint32 DEFAULT_IGNORABLE_CODE_POINT_RUNS[12] = {
	RUN(0, 847), RUN(3, 1564), RUN(5, 4447), RUN(7, 6068), RUN(9, 8203), RUN(13, 12644), RUN(19, 65024), RUN(21, 113824), RUN(29, 119155), RUN(31, 917504), RUN(33, 921600), RUN(34, 2035712)
};

/* DEFAULT_IGNORABLE_CODE_POINT_OFFSETS is core's unicode_data::default_ignorable_code_point's offsets. */
static const uint8 DEFAULT_IGNORABLE_CODE_POINT_OFFSETS[35] = {
	173, 1, 0, 1, 0, 1, 0, 2, 0, 2, 85, 5, 0, 5, 26, 5,
	49, 16, 0, 1, 0, 16, 239, 1, 160, 1, 79, 9, 0, 4, 0, 8,
	0, 0, 0,
};

/* DEFAULT_IGNORABLE_CODE_POINT_THRESHOLD is the first char default_ignorable_code_point::lookup searches for. */
#define DEFAULT_IGNORABLE_CODE_POINT_THRESHOLD 0xad

/* GRAPHEME_EXTEND_RUNS is core's unicode_data::grapheme_extend: its runs, as (start index, prefix sum). */
static const uint32 GRAPHEME_EXTEND_RUNS[33] = {
	RUN(0, 768), RUN(1, 1155), RUN(3, 1425), RUN(5, 4957), RUN(249, 5906), RUN(251, 8204), RUN(347, 11503), RUN(351, 12330), RUN(357, 42607), RUN(361, 43010), RUN(369, 64286), RUN(435, 65024), RUN(437, 65438), RUN(441, 66045), RUN(443, 68097), RUN(449, 68900), RUN(461, 69291), RUN(465, 71727), RUN(601, 73459), RUN(669, 78912), RUN(679, 90398), RUN(683, 92912), RUN(687, 94031), RUN(691, 113821), RUN(699, 118528), RUN(701, 119141), RUN(705, 121344), RUN(717, 122880), RUN(729, 123566), RUN(743, 124140), RUN(747, 125136), RUN(759, 917536), RUN(763, 2032112)
};

/* GRAPHEME_EXTEND_OFFSETS is core's unicode_data::grapheme_extend's offsets. */
static const uint8 GRAPHEME_EXTEND_OFFSETS[767] = {
	0, 112, 0, 7, 0, 45, 1, 1, 1, 2, 1, 2, 1, 1, 72, 11,
	48, 21, 16, 1, 101, 7, 2, 6, 2, 2, 1, 4, 35, 1, 30, 27,
	91, 11, 58, 9, 9, 1, 24, 4, 1, 9, 1, 3, 1, 5, 43, 3,
	59, 9, 42, 24, 1, 32, 55, 1, 1, 1, 4, 8, 4, 1, 3, 7,
	10, 2, 29, 1, 58, 1, 1, 1, 2, 4, 8, 1, 9, 1, 10, 2,
	26, 1, 2, 2, 57, 1, 4, 2, 4, 2, 2, 3, 3, 1, 30, 2,
	3, 1, 11, 2, 57, 1, 4, 5, 1, 2, 4, 1, 20, 2, 22, 6,
	1, 1, 58, 1, 1, 2, 1, 4, 8, 1, 7, 3, 10, 2, 30, 1,
	59, 1, 1, 1, 12, 1, 9, 1, 40, 1, 3, 1, 55, 1, 1, 3,
	5, 3, 1, 4, 7, 2, 11, 2, 29, 1, 58, 1, 2, 2, 1, 1,
	3, 3, 1, 4, 7, 2, 11, 2, 28, 2, 57, 2, 1, 1, 2, 4,
	8, 1, 9, 1, 10, 2, 29, 1, 72, 1, 4, 1, 2, 3, 1, 1,
	8, 1, 81, 1, 2, 7, 12, 8, 98, 1, 2, 9, 11, 7, 73, 2,
	27, 1, 1, 1, 1, 1, 55, 14, 1, 5, 1, 2, 5, 11, 1, 36,
	9, 1, 102, 4, 1, 6, 1, 2, 2, 2, 25, 2, 4, 3, 16, 4,
	13, 1, 2, 2, 6, 1, 15, 1, 0, 3, 0, 4, 28, 3, 29, 2,
	30, 2, 64, 2, 1, 7, 8, 1, 2, 11, 9, 1, 45, 3, 1, 1,
	117, 2, 34, 1, 118, 3, 4, 2, 9, 1, 6, 3, 219, 2, 2, 1,
	58, 1, 1, 7, 1, 1, 1, 1, 2, 8, 6, 10, 2, 1, 48, 46,
	2, 12, 20, 4, 48, 10, 4, 3, 38, 9, 12, 2, 32, 4, 2, 6,
	56, 1, 1, 2, 3, 1, 1, 5, 56, 8, 2, 2, 152, 3, 1, 13,
	1, 7, 4, 1, 6, 1, 3, 2, 198, 64, 0, 1, 195, 33, 0, 3,
	141, 1, 96, 32, 0, 6, 105, 2, 0, 4, 1, 10, 32, 2, 80, 2,
	0, 1, 3, 1, 4, 1, 25, 2, 5, 1, 151, 2, 26, 18, 13, 1,
	38, 8, 25, 11, 1, 1, 44, 3, 48, 1, 2, 4, 2, 2, 2, 1,
	36, 1, 67, 6, 2, 2, 2, 2, 12, 1, 8, 1, 47, 1, 51, 1,
	1, 3, 2, 2, 5, 2, 1, 1, 42, 2, 8, 1, 238, 1, 2, 1,
	4, 1, 0, 1, 0, 16, 16, 16, 0, 2, 0, 1, 226, 1, 149, 5,
	0, 3, 1, 2, 5, 4, 40, 3, 4, 1, 165, 2, 0, 4, 65, 5,
	0, 2, 77, 6, 70, 11, 49, 4, 123, 1, 54, 15, 41, 1, 2, 2,
	10, 3, 49, 4, 2, 2, 7, 1, 61, 3, 36, 5, 1, 8, 62, 1,
	12, 2, 52, 9, 1, 1, 8, 4, 2, 1, 95, 3, 2, 4, 6, 1,
	2, 1, 157, 1, 3, 8, 21, 2, 57, 2, 1, 1, 1, 1, 12, 1,
	9, 1, 14, 7, 3, 5, 67, 1, 2, 6, 1, 1, 2, 1, 1, 3,
	4, 3, 1, 1, 14, 2, 85, 8, 2, 3, 1, 1, 23, 1, 81, 1,
	2, 6, 1, 1, 2, 1, 1, 2, 1, 2, 235, 1, 2, 4, 6, 2,
	1, 2, 27, 2, 85, 8, 2, 1, 1, 2, 106, 1, 1, 1, 2, 8,
	101, 1, 1, 1, 2, 4, 1, 5, 0, 9, 1, 2, 245, 1, 10, 4,
	4, 1, 144, 4, 2, 2, 4, 1, 32, 10, 40, 6, 2, 4, 8, 1,
	9, 6, 2, 3, 46, 13, 1, 2, 198, 1, 1, 3, 1, 1, 201, 7,
	1, 6, 1, 1, 82, 22, 2, 7, 1, 2, 1, 2, 122, 6, 3, 1,
	1, 2, 1, 7, 1, 1, 72, 2, 3, 1, 1, 1, 0, 2, 11, 2,
	52, 5, 5, 3, 23, 1, 0, 1, 6, 15, 0, 12, 3, 3, 0, 5,
	59, 7, 0, 1, 63, 4, 81, 1, 11, 2, 0, 2, 0, 46, 2, 23,
	0, 5, 3, 6, 8, 8, 2, 7, 30, 4, 148, 3, 0, 55, 4, 50,
	8, 1, 14, 1, 22, 5, 1, 15, 0, 7, 1, 17, 2, 7, 1, 2,
	1, 5, 100, 1, 160, 7, 0, 1, 61, 4, 0, 4, 254, 2, 243, 1,
	2, 1, 7, 2, 5, 1, 0, 7, 109, 7, 0, 96, 128, 240, 0,
};

/* GRAPHEME_EXTEND_THRESHOLD is the first char grapheme_extend::lookup searches for. */
#define GRAPHEME_EXTEND_THRESHOLD 0x300

/* WHITESPACE_MAP is core's unicode_data::white_space's map. */
static const uint8 WHITESPACE_MAP[256] = {
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/*
 * std_unicode_skip_search is core's unicode_data::skip_search: whether a char falls in
 * a run of the table, the runs found by binary search on the high bits and
 * the offsets summed within one.
 */
static bool
std_unicode_skip_search(uint32 needle, const uint32 *runs, size_t runs_len,
						const uint8 *offsets, size_t offsets_len)
{
	size_t		lo = 0;
	size_t		hi = runs_len;
	size_t		last_idx;
	size_t		offset_idx;
	size_t		length;
	uint32		prev;
	uint32		total;
	uint32		prefix_sum = 0;

	/*
	 * binary_search_by_key on (needle << 11) against each run's (header <<
	 * 11)
	 */
	while (lo < hi)
	{
		size_t		mid = lo + (hi - lo) / 2;
		uint32		key = runs[mid] << 11;

		if (key == (needle << 11))
		{
			lo = mid + 1;
			hi = lo;
			break;
		}
		else if (key < (needle << 11))
			lo = mid + 1;
		else
			hi = mid;
	}
	last_idx = lo;
	std_index_check(last_idx, runs_len, STD_UNICODE_DATA_FILE, 118, 26);
	offset_idx = RUN_START(runs[last_idx]);
	if (last_idx + 1 < runs_len)
		length = RUN_START(runs[last_idx + 1]) - offset_idx;
	else
		length = offsets_len - offset_idx;
	if (last_idx > 0)
	{
		std_index_check(last_idx - 1, runs_len, STD_UNICODE_DATA_FILE, 126, 44);
		prev = RUN_SUM(runs[last_idx - 1]);
	}
	else
		prev = 0;
	total = needle - prev;
	for (size_t i = 0; i < length - 1; i++)
	{
		std_index_check(offset_idx, offsets_len, STD_UNICODE_DATA_FILE, 138, 22);
		prefix_sum += offsets[offset_idx];
		if (prefix_sum > total)
			break;
		offset_idx++;
	}
	return offset_idx % 2 == 1;
}

/* LOOKUP is a table's lookup: its threshold, then its skip search. */
#define LOOKUP(name, c) \
	((c) >= name##_THRESHOLD && \
	 std_unicode_skip_search((c), name##_RUNS, lengthof(name##_RUNS), name##_OFFSETS, lengthof(name##_OFFSETS)))

/* std_unicode_white_space is core's unicode_data::white_space::lookup, for a char past ASCII. */
static bool
std_unicode_white_space(uint32 c)
{
	switch (c >> 8)
	{
		case 0:
			std_index_check(c & 0xff, lengthof(WHITESPACE_MAP), STD_UNICODE_DATA_FILE, 922, 18);
			return (WHITESPACE_MAP[c & 0xff] & 1) != 0;
		case 22:
			return c == 0x1680;
		case 32:
			std_index_check(c & 0xff, lengthof(WHITESPACE_MAP), STD_UNICODE_DATA_FILE, 924, 19);
			return (WHITESPACE_MAP[c & 0xff] & 2) != 0;
		case 48:
			return c == 0x3000;
		default:
			return false;
	}
}

/* std_char_is_control is char::is_control. */
bool
std_char_is_control(uint32 c)
{
	return c <= 0x1f || (c >= 0x7f && c <= 0x9f);
}

/* std_char_is_private_use is char::is_private_use. */
bool
std_char_is_private_use(uint32 c)
{
	return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) ||
		(c >= 0x100000 && c <= 0x10FFFD);
}

/* std_char_is_whitespace is char::is_whitespace. */
bool
std_char_is_whitespace(uint32 c)
{
	if (c == ' ' || (c >= 0x09 && c <= 0x0d))
		return true;
	if (c <= 0x84)
		return false;
	return std_unicode_white_space(c);
}

/* std_char_is_grapheme_extender is char::is_grapheme_extender. */
bool
std_char_is_grapheme_extender(uint32 c)
{
	return c > 0x02FF && LOOKUP(GRAPHEME_EXTEND, c);
}

/* std_char_is_default_ignorable is char::is_default_ignorable. */
bool
std_char_is_default_ignorable(uint32 c)
{
	return c > 0xAC && LOOKUP(DEFAULT_IGNORABLE_CODE_POINT, c);
}

/* std_char_is_format_control is char::is_format_control. */
bool
std_char_is_format_control(uint32 c)
{
	return c > 0xAC && LOOKUP(CF, c);
}

/* std_char_is_assigned is char::is_assigned. */
bool
std_char_is_assigned(uint32 c)
{
	if (c <= 0x377)
		return true;
	if (c <= 0x3FFFD)
		return !LOOKUP(CN_PLANES_0_3, c);
	return c == 0xE0001 || (c >= 0xE0020 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF) ||
		(c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
}

/*
 * std_char_escape_unicode is escape.rs's escape_unicode::<10>: a char's
 * \\u{NNNN} in a 10-byte buffer, its six hex digits written from the
 * table, returning where the escape starts.
 */
static size_t
std_char_escape_unicode(uint32 c, char *output)
{
	static const char HEX_DIGITS[16] = "0123456789abcdef";
	size_t		start = (size_t) pg_leftmost_one_pos32(c | 1);

	start = (31 - start) / 4 - 2;
	memset(output, 0, 10);
	std_index_check((c >> 20) & 15, 16, STD_ESCAPE_FILE, 147, 17);
	output[3] = HEX_DIGITS[(c >> 20) & 15];
	std_index_check((c >> 16) & 15, 16, STD_ESCAPE_FILE, 148, 17);
	output[4] = HEX_DIGITS[(c >> 16) & 15];
	std_index_check((c >> 12) & 15, 16, STD_ESCAPE_FILE, 149, 17);
	output[5] = HEX_DIGITS[(c >> 12) & 15];
	std_index_check((c >> 8) & 15, 16, STD_ESCAPE_FILE, 150, 17);
	output[6] = HEX_DIGITS[(c >> 8) & 15];
	std_index_check((c >> 4) & 15, 16, STD_ESCAPE_FILE, 151, 17);
	output[7] = HEX_DIGITS[(c >> 4) & 15];
	std_index_check((c >> 0) & 15, 16, STD_ESCAPE_FILE, 152, 17);
	output[8] = HEX_DIGITS[(c >> 0) & 15];
	output[9] = '}';
	std_index_check(start + 0, 10, STD_ESCAPE_FILE, 154, 5);
	output[start + 0] = '\\';
	std_index_check(start + 1, 10, STD_ESCAPE_FILE, 155, 5);
	output[start + 1] = 'u';
	std_index_check(start + 2, 10, STD_ESCAPE_FILE, 156, 5);
	output[start + 2] = '{';
	return start;
}

/*
 * std_char_escape_debug_ext is char::escape_debug_ext: the escape of a char,
 * written into buf, and its length in chars, 1 for a char that needs none.
 */
size_t
std_char_escape_debug_ext(uint32 c, bool escape_grapheme_extender,
						  bool escape_single_quote, bool escape_double_quote,
						  char *buf, size_t *buf_len)
{
	const char *backslash = NULL;

	if (c == '"' && escape_double_quote)
		backslash = "\\\"";
	else if (c == '\'' && escape_single_quote)
		backslash = "\\'";
	else if (c == '\\')
		backslash = "\\\\";
	else if (c == '\n')
		backslash = "\\n";
	else if (c == '\t')
		backslash = "\\t";
	else if (c == '\r')
		backslash = "\\r";
	else if (c == '\0')
		backslash = "\\0";
	if (backslash != NULL)
	{
		memcpy(buf, backslash, 2);
		*buf_len = 2;
		return 2;
	}
	if ((c >= 0x20 && c <= 0x7E) || c == 0xFF9E || c == 0xFF9F)
	{
		*buf_len = 0;
		return 1;
	}
	if (std_char_is_control(c) || std_char_is_private_use(c) || std_char_is_whitespace(c) ||
		(escape_grapheme_extender && std_char_is_grapheme_extender(c)) ||
		std_char_is_default_ignorable(c) || std_char_is_format_control(c) ||
		!std_char_is_assigned(c))
	{
		size_t		start = std_char_escape_unicode(c, buf);

		memmove(buf, buf + start, 10 - start);
		*buf_len = 10 - start;
		return 10 - start;
	}
	*buf_len = 0;
	return 1;
}
