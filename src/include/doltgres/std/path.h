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
 * The C forms of Rust's Path, PathBuf, and their components (the port
 * text's section 9), a port of std/src/path.rs and std/src/sys/path at Rust
 * 1.99.1 with the rules of the target's family: Unix's, and Windows' with
 * its prefixes. A path's bytes are its OsStr's encoded bytes (WTF-8 on
 * Windows); a &Path is a pointer and a length, and a PathBuf a StdVec.
 */
#ifndef DOLTGRES_STD_PATH_H
#define DOLTGRES_STD_PATH_H

#include "doltgres/std/str.h"
#include "doltgres/std/path_layouts.h"
#include "doltgres/std/vec.h"

/*
 * StdPathBuf is a PathBuf: its OsString's encoded bytes and, on Windows,
 * Wtf8Buf's is_known_utf8, whether the bytes are known to be UTF-8.
 */
typedef struct StdPathBuf
{
	StdVec		bytes;
#ifdef WIN32
	bool		is_known_utf8;
#endif
} StdPathBuf;

/* StdPrefixKind is std::path::Prefix's variants, Windows' alone, by their tags in rustc's layout. */
typedef enum StdPrefixKind
{
	STD_PREFIX_VERBATIM = STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM,
	STD_PREFIX_VERBATIM_UNC = STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM_UNC,
	STD_PREFIX_VERBATIM_DISK = STD_PATH_LAYOUT_COMPONENT_TAG_VERBATIM_DISK,
	STD_PREFIX_DEVICE_NS = STD_PATH_LAYOUT_COMPONENT_TAG_DEVICE_NS,
	STD_PREFIX_UNC = STD_PATH_LAYOUT_COMPONENT_TAG_UNC,
	STD_PREFIX_DISK = STD_PATH_LAYOUT_COMPONENT_TAG_DISK,
} StdPrefixKind;

/*
 * StdPrefix is std::path::Prefix at rustc 1.99.1's layout (path_layouts.h):
 * its tag, a StdPrefixKind, a disk's drive, and its first and second OsStrs.
 */
typedef struct StdPrefix
{
	uint8		kind;
	uint8		drive;
	StdStr		a;
	StdStr		b;
} StdPrefix;

/*
 * StdComponentKind is std::path::Component's variants other than Prefix, by
 * their tags in the niche of Prefix's tag.
 */
typedef enum StdComponentKind
{
	STD_COMPONENT_ROOT_DIR = STD_PATH_LAYOUT_COMPONENT_TAG_ROOT_DIR,
	STD_COMPONENT_CUR_DIR = STD_PATH_LAYOUT_COMPONENT_TAG_CUR_DIR,
	STD_COMPONENT_PARENT_DIR = STD_PATH_LAYOUT_COMPONENT_TAG_PARENT_DIR,
	STD_COMPONENT_NORMAL = STD_PATH_LAYOUT_COMPONENT_TAG_NORMAL,
} StdComponentKind;

/*
 * StdComponent is std::path::Component at rustc 1.99.1's layout: a Prefix's
 * parse and raw bytes, the parse's tag a StdPrefixKind, or another variant's
 * StdComponentKind in that tag, with a Normal's bytes as the parse's first
 * OsStr.
 */
typedef struct StdComponent
{
	StdPrefix	parsed;
	StdStr		raw;
} StdComponent;

StaticAssertDecl(sizeof(StdComponent) == STD_PATH_LAYOUT_COMPONENT_SIZE &&
				 _Alignof(StdComponent) == STD_PATH_LAYOUT_COMPONENT_ALIGN &&
				 offsetof(StdComponent, parsed.kind) == STD_PATH_LAYOUT_COMPONENT_TAG &&
				 offsetof(StdComponent, parsed.drive) == STD_PATH_LAYOUT_COMPONENT_DRIVE &&
				 offsetof(StdComponent, parsed.a) == STD_PATH_LAYOUT_COMPONENT_A &&
				 offsetof(StdComponent, parsed.b) == STD_PATH_LAYOUT_COMPONENT_B &&
				 offsetof(StdComponent, raw) == STD_PATH_LAYOUT_COMPONENT_RAW &&
				 offsetof(StdComponent, parsed.a) == STD_PATH_LAYOUT_COMPONENT_NORMAL,
				 "StdComponent is Component at rustc's measured layout");

/* StdComponentsState is Components' State, in its order. */
typedef enum StdComponentsState
{
	STD_STATE_PREFIX = 0,
	STD_STATE_START_DIR = 1,
	STD_STATE_BODY = 2,
	STD_STATE_DONE = 3,
} StdComponentsState;

/* StdComponents is Components: the path left, its prefix, its root, and the two ends' states. */
typedef struct StdComponents
{
	const char *path;
	size_t		path_len;
	bool		has_prefix;
	StdPrefix	prefix;
	bool		has_physical_root;
	StdComponentsState front;
	StdComponentsState back;
} StdComponents;

extern StdComponents std_path_components(const char *path, size_t len);
extern bool std_Components_next(StdComponents *self, StdComponent *result);
extern bool std_Components_next_back(StdComponents *self, StdComponent *result);
extern StdStr std_Component_as_os_str(const StdComponent *self);
extern StdStr std_Components_as_path(const StdComponents *self);
extern bool std_path_is_absolute(const char *path, size_t len);
extern bool std_path_has_root(const char *path, size_t len);
extern bool std_path_parent(const char *path, size_t len, StdStr *result);
extern bool std_path_file_name(const char *path, size_t len, StdStr *result);
extern void std_path_to_path_buf(const char *path, size_t len, StdPathBuf *result);
extern void std_PathBuf_new(StdPathBuf *result);
extern void std_PathBuf_Clone_clone(const StdPathBuf *self, StdPathBuf *result);
extern void std_PathBuf_from_String(StdString *s, StdPathBuf *result);
extern void std_PathBuf_push(StdPathBuf *self, const char *path, size_t len);
extern void std_OsString_push(StdPathBuf *self, const char *bytes, size_t len);
extern bool std_path_file_stem(const char *path, size_t len, StdStr *result);
extern bool std_path_extension(const char *path, size_t len, StdStr *result);
extern bool std_PathBuf_pop(StdPathBuf *self);
extern void std_PathBuf_set_file_name(StdPathBuf *self, const char *file_name, size_t file_name_len);
extern bool std_PathBuf_set_extension(StdPathBuf *self, const char *extension, size_t extension_len);
extern void std_path_with_file_name(const char *path, size_t len, const char *file_name, size_t file_name_len,
									StdPathBuf *result);
extern void std_path_with_extension(const char *path, size_t len, const char *extension, size_t extension_len,
									StdPathBuf *result);
extern bool std_path_PartialEq_eq(const char *path, size_t len, const char *other, size_t other_len);
extern void std_path_join(const char *path, size_t len, const char *other,
						  size_t other_len, StdPathBuf *result);
extern void std_path_to_string_lossy(const char *path, size_t len, StdCowStr *result);

struct StdFormatter;
extern bool std_path_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool std_path_Display_fmt(const void *value, struct StdFormatter *f);
extern bool std_PathBuf_Debug_fmt(const void *value, struct StdFormatter *f);

#endif							/* DOLTGRES_STD_PATH_H */
