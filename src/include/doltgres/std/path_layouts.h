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
 * The layout of std::path::Component (the port text's section 4), whose
 * size sizes a Vec<Component>'s allocations: rustc 1.99.1's size and
 * alignment, the offsets of its tag, of Prefix's drive and two OsStrs, of
 * PrefixComponent's raw, and of Normal's OsStr, and the tag of each variant,
 * Prefix's own tags with Component's other variants in their niche, which
 * path.h's StdComponent takes.
 */
#ifndef DOLTGRES_STD_PATH_LAYOUTS_H
#define DOLTGRES_STD_PATH_LAYOUTS_H

#define STD_PATH_LAYOUT_COMPONENT_SIZE 56
#define STD_PATH_LAYOUT_COMPONENT_ALIGN 8
#define STD_PATH_LAYOUT_COMPONENT_TAG 0
#define STD_PATH_LAYOUT_COMPONENT_DRIVE 1
#define STD_PATH_LAYOUT_COMPONENT_A 8
#define STD_PATH_LAYOUT_COMPONENT_B 24
#define STD_PATH_LAYOUT_COMPONENT_RAW 40
#define STD_PATH_LAYOUT_COMPONENT_NORMAL 8
#define STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM 0
#define STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM_UNC 1
#define STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM_DISK 2
#define STD_PATH_LAYOUT_COMPONENT_TAG_DEVICE_NS 3
#define STD_PATH_LAYOUT_COMPONENT_TAG_UNC 4
#define STD_PATH_LAYOUT_COMPONENT_TAG_DISK 5
#define STD_PATH_LAYOUT_COMPONENT_TAG_ROOT_DIR 6
#define STD_PATH_LAYOUT_COMPONENT_TAG_CUR_DIR 7
#define STD_PATH_LAYOUT_COMPONENT_TAG_PARENT_DIR 8
#define STD_PATH_LAYOUT_COMPONENT_TAG_NORMAL 9

#endif							/* DOLTGRES_STD_PATH_LAYOUTS_H */
