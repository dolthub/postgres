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
 * The C forms of Rust's Path and PathBuf (the port text's section 9), ported
 * from std/src/path.rs, std/src/sys/path/{unix,windows,windows_prefix}.rs,
 * core/src/str/lossy.rs, and core/src/wtf8.rs at Rust 1.99.1, with Unix's
 * rules or Windows' as the target's family has them.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unicode.h"
#include "doltgres/std/vec.h"

/* str::Chars::next as core's own code runs it, with no precondition checks, which str.c defines. */
#define STD_STR_SCOPE extern
#define STD_STR_DECLARE
#define STD_STR_UB_CHECKS 0
#define STD_STR_SUFFIX Unchecked
#include "doltgres/std/str_template.h"

/* PATH_FILE is std's path.rs, where Components' panics and checks are. */
#define PATH_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "path.rs"

/* WINDOWS_PREFIX_FILE is std's sys/path/windows_prefix.rs, where a Windows prefix is parsed. */
#define WINDOWS_PREFIX_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "path" STD_SEP "windows_prefix.rs"

/* STR_LOSSY_FILE is core's str/lossy.rs, where a Utf8Chunk's Debug slices its valid text. */
#define STR_LOSSY_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "lossy.rs"

/* WTF8_FILE is core's wtf8.rs, where Wtf8 finds surrogates and formats its Debug. */
#define WTF8_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "wtf8.rs"

/* ALLOC_WTF8_FILE is alloc's wtf8/mod.rs, where Wtf8Buf pushes a slice. */
#define ALLOC_WTF8_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "wtf8" STD_SEP "mod.rs"

/* EXACT_SIZE_FILE is core's iter/traits/exact_size.rs, where ExactSizeIterator::len asserts its size_hint. */
#define EXACT_SIZE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "traits" STD_SEP "exact_size.rs"

/* OS_STR_FILE is std's ffi/os_str.rs, whose OsString::truncate checks its boundary. */
#define OS_STR_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "ffi" STD_SEP "os_str.rs"

/* STD_PATH_UNREACHABLE is unreachable!()'s message. */
#define STD_PATH_UNREACHABLE "internal error: entered unreachable code"

#ifdef WIN32
#define HAS_PREFIXES true
#define MAIN_SEPARATOR_STR "\\"
#else
#define HAS_PREFIXES false
#define MAIN_SEPARATOR_STR "/"
#endif

/* std_path_is_sep_byte is a byte that separates a path's components. */
static bool
std_path_is_sep_byte(uint8 b)
{
#ifdef WIN32
	return b == '/' || b == '\\';
#else
	return b == '/';
#endif
}

/* std_path_is_verbatim_sep is a separator in a verbatim path, which only '\\' is on Windows. */
static bool
std_path_is_verbatim_sep(uint8 b)
{
#ifdef WIN32
	return b == '\\';
#else
	return b == '/';
#endif
}

#ifdef WIN32
/*
 * std_path_parse_next_component splits a path at its first separator, verbatim's or
 * not, into the component and the rest, the rest empty when there is none.
 */
static void
std_path_parse_next_component(StdStr path, bool verbatim, StdStr *component,
							  StdStr *rest)
{
	for (size_t i = 0; i < path.len; i++)
	{
		uint8		b = (uint8) path.ptr[i];

		if (verbatim ? std_path_is_verbatim_sep(b) : std_path_is_sep_byte(b))
		{
			std_range_to_check(i, path.len, WINDOWS_PREFIX_FILE, 163, 53);
			std_range_from_check(i + 1, path.len, WINDOWS_PREFIX_FILE, 167, 48);
			*component = (StdStr)
			{
				path.ptr, i
			};
			*rest = (StdStr)
			{
				path.ptr + i + 1, path.len - i - 1
			};
			return;
		}
	}
	*component = path;
	*rest = (StdStr)
	{
		path.ptr + path.len, 0
	};
}

/* std_path_is_ascii_alphabetic is u8::is_ascii_alphabetic. */
static bool
std_path_is_ascii_alphabetic(uint8 c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* std_path_parse_drive is a drive letter and a colon, the letter upper-cased, or none. */
static bool
std_path_parse_drive(StdStr path, uint8 *drive)
{
	if (path.len >= 2 && path.ptr[1] == ':' && std_path_is_ascii_alphabetic((uint8) path.ptr[0]))
	{
		uint8		c = (uint8) path.ptr[0];

		*drive = (c >= 'a' && c <= 'z') ? (uint8) (c - 32) : c;
		return true;
	}
	return false;
}

/* std_path_parse_drive_exact is a drive that is the whole path or is followed by a separator. */
static bool
std_path_parse_drive_exact(StdStr path, uint8 *drive)
{
	if (path.len <= 2 || std_path_is_sep_byte((uint8) path.ptr[2]))
		return std_path_parse_drive(path, drive);
	return false;
}

/*
 * std_path_strip_at is PrefixParserSlice::strip_prefix: whether the first eight
 * bytes, with '/' read as '\\', hold the text at an index, by
 * <[u8]>::starts_with of the bytes from the index.
 */
static bool
std_path_strip_at(const char *prefix8, size_t prefix_len, size_t index, const char *text)
{
	size_t		n = strlen(text);
	size_t		rest;

	std_range_from_check(index, prefix_len, WINDOWS_PREFIX_FILE, 44, 20);
	rest = prefix_len - index;
	if (!(rest >= n))
		return false;
	std_range_to_check(n, rest, STD_SLICE_MOD_FILE, 2635, 43);
	return memcmp(prefix8 + index, text, n) == 0;
}

/*
 * std_path_prefix_bytes_have_slash is whether a prefix parser's
 * prefix_bytes, the path's bytes before its index, hold a '/', as
 * iter().any(|&x| x == b'/') looks byte by byte.
 */
static bool
std_path_prefix_bytes_have_slash(const char *path, size_t len, size_t index)
{
	std_range_to_check(index, len, WINDOWS_PREFIX_FILE, 50, 38);
	for (size_t i = 0; i < index; i++)
		if (path[i] == '/')
			return true;
	return false;
}

/* std_path_parse_prefix is Windows' parse_prefix: a path's prefix, or none. */
static bool
std_path_parse_prefix(const char *path, size_t len, StdPrefix *prefix)
{
	char		p[8] = {0};
	size_t		plen = std_min_usize(len, 8);
	size_t		index = 0;
	StdStr		rest;
	StdStr		server;
	StdStr		share;
	StdStr		ignored;

	for (size_t i = 0; i < plen; i++)
	{
		std_index_check(i, sizeof(p), WINDOWS_PREFIX_FILE, 18, 13);
		p[i] = path[i] == '/' ? '\\' : path[i];
	}
	std_range_to_check(plen, 8, WINDOWS_PREFIX_FILE, 30, 33);
	if (std_path_strip_at(p, plen, index, "\\\\"))
	{
		index += 2;
		if (std_path_strip_at(p, plen, index, "?\\") && !std_path_prefix_bytes_have_slash(path, len, index + 2))
		{
			index += 2;
			if (std_path_strip_at(p, plen, index, "UNC\\"))
			{
				index += 4;
				std_range_from_check(index, len, WINDOWS_PREFIX_FILE, 58, 83);
				rest = (StdStr)
				{
					path + index, len - index
				};
				std_path_parse_next_component(rest, true, &server, &rest);
				std_path_parse_next_component(rest, true, &share, &ignored);
				prefix->kind = STD_PREFIX_VERBATIM_UNC;
				prefix->drive = 0;
				prefix->a = server;
				prefix->b = share;
				return true;
			}
			std_range_from_check(index, len, WINDOWS_PREFIX_FILE, 58, 83);
			rest = (StdStr)
			{
				path + index, len - index
			};
			if (std_path_parse_drive_exact(rest, &prefix->drive))
			{
				prefix->kind = STD_PREFIX_VERBATIM_DISK;
				prefix->a = prefix->b = (StdStr)
				{
					NULL, 0
				};
				return true;
			}
			std_path_parse_next_component(rest, true, &server, &ignored);
			prefix->kind = STD_PREFIX_VERBATIM;
			prefix->drive = 0;
			prefix->a = server;
			prefix->b.ptr = NULL;
			prefix->b.len = 0;
			return true;
		}
		else if (std_path_strip_at(p, plen, index, ".\\"))
		{
			index += 2;
			std_range_from_check(index, len, WINDOWS_PREFIX_FILE, 58, 83);
			rest = (StdStr)
			{
				path + index, len - index
			};
			std_path_parse_next_component(rest, false, &server, &ignored);
			prefix->kind = STD_PREFIX_DEVICE_NS;
			prefix->drive = 0;
			prefix->a = server;
			prefix->b.ptr = NULL;
			prefix->b.len = 0;
			return true;
		}
		std_range_from_check(index, len, WINDOWS_PREFIX_FILE, 58, 83);
		rest = (StdStr)
		{
			path + index, len - index
		};
		std_path_parse_next_component(rest, false, &server, &rest);
		std_path_parse_next_component(rest, false, &share, &ignored);
		if (server.len > 0 && share.len > 0)
		{
			prefix->kind = STD_PREFIX_UNC;
			prefix->drive = 0;
			prefix->a = server;
			prefix->b = share;
			return true;
		}
		return false;
	}
	rest = (StdStr)
	{
		path, len
	};
	if (std_path_parse_drive(rest, &prefix->drive))
	{
		prefix->kind = STD_PREFIX_DISK;
		prefix->a = prefix->b = (StdStr)
		{
			NULL, 0
		};
		return true;
	}
	return false;
}
#endif

/* std_path_prefix_len is Prefix::len: how many bytes the prefix takes. */
static size_t
std_path_prefix_len(const StdPrefix *p)
{
	switch (p->kind)
	{
		case STD_PREFIX_VERBATIM:
			return 4 + p->a.len;
		case STD_PREFIX_VERBATIM_UNC:
			return 8 + p->a.len + (p->b.len > 0 ? 1 + p->b.len : 0);
		case STD_PREFIX_VERBATIM_DISK:
			return 6;
		case STD_PREFIX_UNC:
			return 2 + p->a.len + (p->b.len > 0 ? 1 + p->b.len : 0);
		case STD_PREFIX_DEVICE_NS:
			return 4 + p->a.len;
		case STD_PREFIX_DISK:
			return 2;
	}
	pg_unreachable();
}

/* std_path_prefix_is_verbatim is Prefix::is_verbatim. */
static bool
std_path_prefix_is_verbatim(const StdPrefix *p)
{
	return p->kind == STD_PREFIX_VERBATIM || p->kind == STD_PREFIX_VERBATIM_DISK ||
		p->kind == STD_PREFIX_VERBATIM_UNC;
}

/* std_path_components_prefix_len is Components::prefix_len. */
static size_t
std_path_components_prefix_len(const StdComponents *self)
{
	if (!HAS_PREFIXES)
		return 0;
	return self->has_prefix ? std_path_prefix_len(&self->prefix) : 0;
}

/* std_path_components_prefix_verbatim is Components::prefix_verbatim. */
static bool
std_path_components_prefix_verbatim(const StdComponents *self)
{
	if (!HAS_PREFIXES)
		return false;
	return self->has_prefix && std_path_prefix_is_verbatim(&self->prefix);
}

/* std_path_prefix_remaining is how much of the prefix the front hasn't consumed. */
static size_t
std_path_prefix_remaining(const StdComponents *self)
{
	if (!HAS_PREFIXES)
		return 0;
	return self->front == STD_STATE_PREFIX ? std_path_components_prefix_len(self) : 0;
}

/* std_path_components_is_sep_byte is a separator by the path's verbatim rule. */
static bool
std_path_components_is_sep_byte(const StdComponents *self, uint8 b)
{
	return std_path_components_prefix_verbatim(self) ? std_path_is_verbatim_sep(b) : std_path_is_sep_byte(b);
}

/* std_path_components_has_root is Components::has_root. */
static bool
std_path_components_has_root(const StdComponents *self)
{
	if (self->has_physical_root)
		return true;
	if (HAS_PREFIXES && self->has_prefix && self->prefix.kind != STD_PREFIX_DISK)
		return true;
	return false;
}

/* std_path_include_cur_dir is whether the path starts with a "." kept as CurDir. */
static bool
std_path_include_cur_dir(const StdComponents *self)
{
	size_t		start;
	size_t		len;

	if (std_path_components_has_root(self))
		return false;
	start = std_path_prefix_remaining(self);
	std_range_from_check(start, self->path_len, PATH_FILE, 757, 31);
	len = self->path_len - start;
	if (len == 1 && self->path[start] == '.')
		return true;
	if (len >= 2 && self->path[start] == '.')
		return std_path_components_is_sep_byte(self, (uint8) self->path[start + 1]);
	return false;
}

/* std_path_len_before_body is how much of the path before the body is left. */
static size_t
std_path_len_before_body(const StdComponents *self)
{
	size_t		root = (self->front <= STD_STATE_START_DIR && self->has_physical_root) ? 1 : 0;
	size_t		cur_dir = (self->front <= STD_STATE_START_DIR && std_path_include_cur_dir(self)) ? 1 : 0;

	return std_path_prefix_remaining(self) + root + cur_dir;
}

/* std_path_finished is whether the iteration is complete. */
static bool
std_path_finished(const StdComponents *self)
{
	return self->front == STD_STATE_DONE || self->back == STD_STATE_DONE || self->front > self->back;
}

/* std_path_parse_single_component is a component of bytes, or none for "" and a normalized ".". */
static bool
std_path_parse_single_component(const StdComponents *self, StdStr comp, StdComponent *result)
{
	if (comp.len == 1 && comp.ptr[0] == '.')
	{
		if (HAS_PREFIXES && std_path_components_prefix_verbatim(self))
		{
			result->parsed.kind = STD_COMPONENT_CUR_DIR;
			return true;
		}
		return false;
	}
	if (comp.len == 2 && comp.ptr[0] == '.' && comp.ptr[1] == '.')
	{
		result->parsed.kind = STD_COMPONENT_PARENT_DIR;
		return true;
	}
	if (comp.len == 0)
		return false;
	result->parsed.kind = STD_COMPONENT_NORMAL;
	result->parsed.a = comp;
	return true;
}

/* std_path_parse_front parses a component from the left, returning how many bytes it takes. */
static size_t
std_path_parse_front(const StdComponents *self, bool *some, StdComponent *comp)
{
	size_t		extra = 0;
	StdStr		part = {self->path, self->path_len};

	for (size_t i = 0; i < self->path_len; i++)
	{
		if (std_path_components_is_sep_byte(self, (uint8) self->path[i]))
		{
			extra = 1;
			std_range_to_check(i, self->path_len, PATH_FILE, 785, 38);
			part.len = i;
			break;
		}
	}
	*some = std_path_parse_single_component(self, part, comp);
	return part.len + extra;
}

/* std_path_parse_back parses a component from the right, returning how many bytes it takes. */
static size_t
std_path_parse_back(const StdComponents *self, bool *some, StdComponent *comp)
{
	size_t		start = std_path_len_before_body(self);
	size_t		extra = 0;
	StdStr		part;
	bool		found = false;
	size_t		i;

	std_range_from_check(start, self->path_len, PATH_FILE, 796, 44);
	for (i = self->path_len - start; i > 0;)
	{
		i--;
		if (std_path_components_is_sep_byte(self, (uint8) self->path[start + i]))
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		std_range_from_check(start, self->path_len, PATH_FILE, 797, 35);
		part.ptr = self->path + start;
		part.len = self->path_len - start;
	}
	else
	{
		extra = 1;
		std_range_from_check(start + i + 1, self->path_len, PATH_FILE, 798, 38);
		part.ptr = self->path + start + i + 1;
		part.len = self->path_len - (start + i + 1);
	}
	*some = std_path_parse_single_component(self, part, comp);
	return part.len + extra;
}

/* std_path_trim_left trims away repeated separators on the left. */
static void
std_path_trim_left(StdComponents *self)
{
	while (self->path_len > 0)
	{
		bool		some;
		StdComponent comp;
		size_t		size = std_path_parse_front(self, &some, &comp);

		if (some)
			return;
		std_range_from_check(size, self->path_len, PATH_FILE, 811, 39);
		self->path += size;
		self->path_len -= size;
	}
}

/* std_path_trim_right trims away repeated separators on the right. */
static void
std_path_trim_right(StdComponents *self)
{
	while (self->path_len > std_path_len_before_body(self))
	{
		bool		some;
		StdComponent comp;
		size_t		size = std_path_parse_back(self, &some, &comp);

		if (some)
			return;
		std_range_to_check(self->path_len - size, self->path_len, PATH_FILE, 823, 39);
		self->path_len -= size;
	}
}

/* std_path_has_physical_root is has_physical_root: whether the first byte after the prefix is a separator. */
static bool
std_path_has_physical_root(const char *s, size_t len, bool has_prefix, const StdPrefix *prefix)
{
	const char *path = s;
	size_t		path_len = len;

	if (has_prefix)
	{
		size_t		start = std_path_prefix_len(prefix);

		std_range_from_check(start, len, PATH_FILE, 328, 44);
		path = s + start;
		path_len = len - start;
	}
	if (path_len == 0)
		return false;
	std_index_check(0, path_len, PATH_FILE, 329, 37);
	return std_path_is_sep_byte((uint8) path[0]);
}

/* std_path_components is Path::components. */
StdComponents
std_path_components(const char *path, size_t len)
{
	StdComponents c = {path, len, false, {0}, false,
	HAS_PREFIXES ? STD_STATE_PREFIX : STD_STATE_START_DIR, STD_STATE_BODY};

#ifdef WIN32
	c.has_prefix = std_path_parse_prefix(path, len, &c.prefix);
#endif
	c.has_physical_root = std_path_has_physical_root(path, len, c.has_prefix, &c.prefix);
	return c;
}

/* std_Components_next is Components::next. */
bool
std_Components_next(StdComponents *self, StdComponent *result)
{
	while (!std_path_finished(self))
	{
		if (self->front == STD_STATE_BODY && self->path_len > 0)
		{
			bool		some;
			size_t		size = std_path_parse_front(self, &some, result);

			std_range_from_check(size, self->path_len, PATH_FILE, 929, 43);
			self->path += size;
			self->path_len -= size;
			if (some)
				return true;
		}
		else if (self->front == STD_STATE_BODY)
			self->front = STD_STATE_DONE;
		else if (self->front == STD_STATE_START_DIR)
		{
			self->front = STD_STATE_BODY;
			if (self->has_physical_root)
			{
				std_range_from_check(1, self->path_len, PATH_FILE, 941, 47);
				self->path += 1;
				self->path_len -= 1;
				result->parsed.kind = STD_COMPONENT_ROOT_DIR;
				return true;
			}
			else if (HAS_PREFIXES && self->has_prefix)
			{
				if (self->prefix.kind != STD_PREFIX_DISK && !std_path_prefix_is_verbatim(&self->prefix))
				{
					result->parsed.kind = STD_COMPONENT_ROOT_DIR;
					return true;
				}
			}
			else if (std_path_include_cur_dir(self))
			{
				std_range_from_check(1, self->path_len, PATH_FILE, 949, 47);
				self->path += 1;
				self->path_len -= 1;
				result->parsed.kind = STD_COMPONENT_CUR_DIR;
				return true;
			}
		}
#ifndef WIN32
		else if (!HAS_PREFIXES)
			std_panic(PATH_FILE, 953, 49, STD_PATH_UNREACHABLE, sizeof(STD_PATH_UNREACHABLE) - 1);
#endif
		else if (self->front == STD_STATE_PREFIX && std_path_components_prefix_len(self) == 0)
			self->front = STD_STATE_START_DIR;
		else if (self->front == STD_STATE_PREFIX)
		{
			StdStr		raw;

			self->front = STD_STATE_START_DIR;
			std_range_to_check(std_path_components_prefix_len(self), self->path_len, PATH_FILE, 960, 41);
			raw.ptr = self->path;
			raw.len = std_path_components_prefix_len(self);
			{
				size_t		n = std_path_components_prefix_len(self);

				std_range_from_check(n, self->path_len, PATH_FILE, 961, 43);
				self->path += n;
				self->path_len -= n;
			}
			result->raw = raw;
			if (!self->has_prefix)
				std_option_unwrap_failed(PATH_FILE, 964, 45);
			result->parsed = self->prefix;
			return true;
		}
		else
			std_panic(PATH_FILE, 967, 32, STD_PATH_UNREACHABLE, sizeof(STD_PATH_UNREACHABLE) - 1);
	}
	return false;
}

/* std_Components_next_back is Components::next_back. */
bool
std_Components_next_back(StdComponents *self, StdComponent *result)
{
	while (!std_path_finished(self))
	{
		if (self->back == STD_STATE_BODY && self->path_len > std_path_len_before_body(self))
		{
			bool		some;
			size_t		size = std_path_parse_back(self, &some, result);

			std_range_to_check(self->path_len - size, self->path_len, PATH_FILE, 981, 43);
			self->path_len -= size;
			if (some)
				return true;
		}
		else if (self->back == STD_STATE_BODY)
			self->back = STD_STATE_START_DIR;
		else if (self->back == STD_STATE_START_DIR)
		{
			self->back = HAS_PREFIXES ? STD_STATE_PREFIX : STD_STATE_DONE;
			if (self->has_physical_root)
			{
				std_range_to_check(self->path_len - 1, self->path_len, PATH_FILE, 992, 47);
				self->path_len -= 1;
				result->parsed.kind = STD_COMPONENT_ROOT_DIR;
				return true;
			}
			else if (HAS_PREFIXES && self->has_prefix)
			{
				if (self->prefix.kind != STD_PREFIX_DISK && !std_path_prefix_is_verbatim(&self->prefix))
				{
					result->parsed.kind = STD_COMPONENT_ROOT_DIR;
					return true;
				}
			}
			else if (std_path_include_cur_dir(self))
			{
				std_range_to_check(self->path_len - 1, self->path_len, PATH_FILE, 999, 47);
				self->path_len -= 1;
				result->parsed.kind = STD_COMPONENT_CUR_DIR;
				return true;
			}
		}
#ifndef WIN32
		else if (!HAS_PREFIXES)
			std_panic(PATH_FILE, 1003, 39, STD_PATH_UNREACHABLE, sizeof(STD_PATH_UNREACHABLE) - 1);
#endif
		else if (self->back == STD_STATE_PREFIX && std_path_components_prefix_len(self) > 0)
		{
			self->back = STD_STATE_DONE;
			result->raw.ptr = self->path;
			result->raw.len = self->path_len;
			if (!self->has_prefix)
				std_option_unwrap_failed(PATH_FILE, 1008, 45);
			result->parsed = self->prefix;
			return true;
		}
		else if (self->back == STD_STATE_PREFIX)
		{
			self->back = STD_STATE_DONE;
			return false;
		}
		else
			std_panic(PATH_FILE, 1015, 32, STD_PATH_UNREACHABLE, sizeof(STD_PATH_UNREACHABLE) - 1);
	}
	return false;
}

/*
 * std_Component_as_os_str is Component::as_os_str: a prefix's text, the
 * separator for the root, "." and "..", and a normal component's text.
 */
StdStr
std_Component_as_os_str(const StdComponent *self)
{
	StdStr		result;

	switch (self->parsed.kind)
	{
		case STD_PREFIX_VERBATIM:
		case STD_PREFIX_VERBATIM_UNC:
		case STD_PREFIX_VERBATIM_DISK:
		case STD_PREFIX_DEVICE_NS:
		case STD_PREFIX_UNC:
		case STD_PREFIX_DISK:
			return self->raw;
		case STD_COMPONENT_NORMAL:
			return self->parsed.a;
		case STD_COMPONENT_ROOT_DIR:
			result.ptr = MAIN_SEPARATOR_STR;
			result.len = 1;
			return result;
		case STD_COMPONENT_CUR_DIR:
			result.ptr = ".";
			result.len = 1;
			return result;
		case STD_COMPONENT_PARENT_DIR:
			result.ptr = "..";
			result.len = 2;
			return result;
	}
	pg_unreachable();
}

/* std_Components_as_path is Components::as_path: the path the iteration has left. */
StdStr
std_Components_as_path(const StdComponents *self)
{
	StdComponents comps = *self;

	if (comps.front == STD_STATE_BODY)
		std_path_trim_left(&comps);
	if (comps.back == STD_STATE_BODY)
		std_path_trim_right(&comps);
	return (StdStr)
	{
		comps.path, comps.path_len
	};
}

/* std_path_has_root is Path::has_root. */
bool
std_path_has_root(const char *path, size_t len)
{
	StdComponents c = std_path_components(path, len);

	return std_path_components_has_root(&c);
}

/* std_path_is_absolute is Path::is_absolute: a root, and on Windows a prefix too. */
bool
std_path_is_absolute(const char *path, size_t len)
{
#ifdef WIN32
	StdComponents c = std_path_components(path, len);

	return std_path_components_has_root(&c) && c.has_prefix;
#else
	return std_path_has_root(path, len);
#endif
}

/* std_path_parent is Path::parent: the path without its last component, or none. */
bool
std_path_parent(const char *path, size_t len, StdStr *result)
{
	StdComponents comps = std_path_components(path, len);
	StdComponent comp;

	if (!std_Components_next_back(&comps, &comp))
		return false;
	switch (comp.parsed.kind)
	{
		case STD_COMPONENT_NORMAL:
		case STD_COMPONENT_CUR_DIR:
		case STD_COMPONENT_PARENT_DIR:
			*result = std_Components_as_path(&comps);
			return true;
		default:
			return false;
	}
}

/* std_path_file_name is Path::file_name: the last component when it is a normal one. */
bool
std_path_file_name(const char *path, size_t len, StdStr *result)
{
	StdComponents comps = std_path_components(path, len);
	StdComponent comp;

	if (std_Components_next_back(&comps, &comp) && comp.parsed.kind == STD_COMPONENT_NORMAL)
	{
		*result = comp.parsed.a;
		return true;
	}
	return false;
}

/*
 * std_path_to_path_buf is Path::to_path_buf, a copy of its bytes: on
 * Windows Wtf8::to_owned's, not known to be UTF-8.
 */
void
std_path_to_path_buf(const char *path, size_t len, StdPathBuf *result)
{
	std_string_from_Unchecked(&result->bytes, path, len);
#ifdef WIN32
	result->is_known_utf8 = false;
#endif
}

/* std_PathBuf_new is PathBuf::new through OsString::new: no bytes, on Windows known to be UTF-8. */
void
std_PathBuf_new(StdPathBuf *result)
{
	std_vec_new(&result->bytes, 1, 1);
#ifdef WIN32
	result->is_known_utf8 = true;
#endif
}

/* std_PathBuf_Clone_clone is PathBuf's Clone through OsString's: a copy of the bytes, and on Windows of whether they are known to be UTF-8. */
void
std_PathBuf_Clone_clone(const StdPathBuf *self, StdPathBuf *result)
{
	std_vec_clone(&result->bytes, &self->bytes, 1, 1);
#ifdef WIN32
	result->is_known_utf8 = self->is_known_utf8;
#endif
}

/* std_PathBuf_from_String is PathBuf's From<String> through OsString's: the String's bytes, on Windows known to be UTF-8; it takes the String. */
void
std_PathBuf_from_String(StdString *s, StdPathBuf *result)
{
	result->bytes = *s;
#ifdef WIN32
	result->is_known_utf8 = true;
#endif
}

/* std_path_os_string_clear is OsString::clear: no bytes left, on Windows Wtf8Buf::clear's, known to be UTF-8. */
static void
std_path_os_string_clear(StdPathBuf *self)
{
	std_vec_clear(&self->bytes, 1, NULL);
#ifdef WIN32
	self->is_known_utf8 = true;
#endif
}

#ifdef WIN32
/* std_path_decode_surrogate is Wtf8's decode_surrogate: the surrogate a lone surrogate's second and third bytes encode. */
static uint16
std_path_decode_surrogate(uint8 second_byte, uint8 third_byte)
{
	return (uint16) (0xD800 | ((uint16) (second_byte & 0x3F)) << 6 | ((uint16) third_byte & 0x3F));
}

/* std_path_decode_surrogate_pair is Wtf8Buf's decode_surrogate_pair: the char a lead and a trail surrogate make. */
static uint32
std_path_decode_surrogate_pair(uint16 lead, uint16 trail)
{
	return 0x10000 + (((uint32) (uint16) (lead - 0xD800) << 10) | (uint32) (uint16) (trail - 0xDC00));
}

/* std_path_final_lead_surrogate is Wtf8::final_lead_surrogate: the lead surrogate the bytes end in, or none. */
static bool
std_path_final_lead_surrogate(const uint8 *bytes, size_t len, uint16 *result)
{
	if (len >= 3 && bytes[len - 3] == 0xED && bytes[len - 2] >= 0xA0 && bytes[len - 2] <= 0xAF)
	{
		*result = std_path_decode_surrogate(bytes[len - 2], bytes[len - 1]);
		return true;
	}
	return false;
}

/* std_path_initial_trail_surrogate is Wtf8::initial_trail_surrogate: the trail surrogate the bytes start with, or none. */
static bool
std_path_initial_trail_surrogate(const uint8 *bytes, size_t len, uint16 *result)
{
	if (len >= 3 && bytes[0] == 0xED && bytes[1] >= 0xB0 && bytes[1] <= 0xBF)
	{
		*result = std_path_decode_surrogate(bytes[1], bytes[2]);
		return true;
	}
	return false;
}

/*
 * std_path_next_surrogate is Wtf8::next_surrogate: the first lone surrogate
 * at or after a position, with its position, or none, stepping over each
 * code point by its first byte.
 */
static bool
std_path_next_surrogate(const uint8 *bytes, size_t len, size_t pos, size_t *result_0, uint16 *result_1)
{
	const uint8 *iter;
	const uint8 *end = bytes + len;

	std_range_from_check(pos, len, WTF8_FILE, 291, 34);
	iter = bytes + pos;
	for (;;)
	{
		uint8		b;

		if (iter == end)
			return false;
		b = *iter++;
		if (b < 0x80)
			pos += 1;
		else if (b < 0xE0)
		{
			if (iter != end)
				iter++;
			pos += 2;
		}
		else if (b == 0xED)
		{
			const uint8 *b2 = iter != end ? iter++ : NULL;
			const uint8 *b3 = iter != end ? iter++ : NULL;

			if (b2 != NULL && b3 != NULL && *b2 >= 0xA0)
			{
				*result_0 = pos;
				*result_1 = std_path_decode_surrogate(*b2, *b3);
				return true;
			}
			pos += 3;
		}
		else if (b < 0xF0)
		{
			for (int k = 0; k < 2 && iter != end; k++)
				iter++;
			pos += 3;
		}
		else
		{
			for (int k = 0; k < 3 && iter != end; k++)
				iter++;
			pos += 4;
		}
	}
}

/* std_path_push_char is Wtf8Buf::push_char through push_code_point_unchecked: the char's UTF-8 appended. */
static void
std_path_push_char(StdPathBuf *self, uint32 c)
{
	uint8		bytes[4] = {0};
	size_t		len = std_str_encode_utf8_Unchecked(c, bytes, sizeof(bytes));

	std_vec_extend_from_slice_Unchecked(&self->bytes, bytes, len, 1, 1);
}
#endif

/*
 * std_OsString_push is OsString::push: on Windows, Wtf8Buf::push_wtf8,
 * which joins a trailing lead surrogate with a leading trail surrogate into
 * one code point and otherwise appends the bytes, no longer known to be
 * UTF-8 once they hold a surrogate; elsewhere, the bytes appended.
 */
void
std_OsString_push(StdPathBuf *self, const char *bytes, size_t len)
{
#ifdef WIN32
	uint16		lead;
	uint16		trail;
	bool		has_lead = std_path_final_lead_surrogate((const uint8 *) self->bytes.ptr, self->bytes.len, &lead);
	bool		has_trail = std_path_initial_trail_surrogate((const uint8 *) bytes, len, &trail);

	if (has_lead && has_trail)
	{
		size_t		len_without_lead_surrogate = self->bytes.len - 3;

		std_vec_truncate(&self->bytes, len_without_lead_surrogate, 1, NULL);
		std_range_from_check(3, len, ALLOC_WTF8_FILE, 292, 70);
		std_vec_reserve_Unchecked(&self->bytes, 4 + (len - 3), 1, 1);
		std_path_push_char(self, std_path_decode_surrogate_pair(lead, trail));
		std_vec_extend_from_slice_Unchecked(&self->bytes, bytes + 3, len - 3, 1, 1);
		return;
	}
	if (self->is_known_utf8)
	{
		size_t		pos;
		uint16		surrogate;

		if (std_path_next_surrogate((const uint8 *) bytes, len, 0, &pos, &surrogate))
			self->is_known_utf8 = false;
	}
#endif
	std_vec_extend_from_slice_Unchecked(&self->bytes, bytes, len, 1, 1);
}

/*
 * std_path_byte_index_panic panics with "byte index {index} " and a
 * reason, as the boundary checks' panic! do, at their caller's location.
 */
pg_noreturn static void
std_path_byte_index_panic(size_t index, const char *reason, size_t reason_len, const char *file, uint32 line,
						  uint32 col)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("byte index "), STD_FMT_ARG(0), {reason, reason_len, 0, {0}}};
	StdFmtArg	args[] = {{&index, std_usize_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(file, line, col, &arguments);
}

#ifdef WIN32
/*
 * std_path_check_public_boundary is the Windows Slice's
 * check_public_boundary, Wtf8::check_utf8_boundary through
 * try_check_utf8_boundary: an index at a code point's edge and not between
 * surrogates, or a panic at the caller's location.
 */
static void
std_path_check_public_boundary(const uint8 *bytes, size_t len, size_t index, const char *file, uint32 line,
							   uint32 col)
{
	static const char not_a_boundary[] = " is not a codepoint boundary";
	static const char out_of_bounds[] = " is out of bounds";
	static const char between[] = " lies between surrogate codepoints";

	if (index == 0)
		return;
	if (index < len)
	{
		if (bytes[index] != 0xED)
		{
			if ((int8) bytes[index] >= -0x40)
				return;
			std_path_byte_index_panic(index, not_a_boundary, sizeof(not_a_boundary) - 1, file, line, col);
		}
	}
	else if (index == len)
		return;
	else
		std_path_byte_index_panic(index, out_of_bounds, sizeof(out_of_bounds) - 1, file, line, col);
	std_index_check(index + 1, len, file, line, col);
	if (bytes[index + 1] >= 0xA0 && index >= 3)
	{
		std_index_check(index - 3, len, file, line, col);
		if (bytes[index - 3] == 0xED)
		{
			std_index_check(index - 2, len, file, line, col);
			if (bytes[index - 2] >= 0xA0)
				std_path_byte_index_panic(index, between, sizeof(between) - 1, file, line, col);
		}
	}
}

/*
 * std_path_wtf8_is_code_point_boundary is Wtf8::is_code_point_boundary:
 * whether an index is at a code point's edge.
 */
static bool
std_path_wtf8_is_code_point_boundary(const uint8 *bytes, size_t len, size_t index)
{
	if (index == 0)
		return true;
	if (index >= len)
		return index == len;
	return (int8) bytes[index] >= -0x40;
}
#else

/*
 * std_path_slow_path is the Unix Slice's try_check_public_boundary's
 * slow_path: an index valid UTF-8 starts at, or one at most four bytes
 * before which valid UTF-8 ends.
 */
static bool
std_path_slow_path(const uint8 *bytes, size_t len, size_t index, const char *file, uint32 line, uint32 col)
{
	const uint8 *after;
	size_t		after_len;
	StdUtf8Error error;

	if (index > len)
		return false;
	after = bytes + index;
	after_len = std_min_usize(len - index, 4);
	if (std_str_from_utf8(after, after_len, &error))
		return true;
	if (error.valid_up_to != 0)
		return true;
	for (size_t n = 2, last = std_min_usize(4, index); n <= last; n++)
	{
		std_range_from_check(index - n, index, file, line, col);
		if (std_str_from_utf8(bytes + index - n, n, &error))
			return true;
	}
	return false;
}

/*
 * std_path_check_public_boundary is the Unix Slice's check_public_boundary
 * through try_check_public_boundary: an index at the ends, beside an ASCII
 * byte, or that the slow path allows, or a panic at the caller's location.
 */
static void
std_path_check_public_boundary(const uint8 *bytes, size_t len, size_t index, const char *file, uint32 line,
							   uint32 col)
{
	static const char reason[] = " is not an OsStr boundary";

	if (index == 0 || index == len)
		return;
	if (index < len)
	{
		std_index_check(index - 1, len, file, line, col);
		if (bytes[index - 1] < 0x80)
			return;
		std_index_check(index, len, file, line, col);
		if (bytes[index] < 0x80)
			return;
	}
	if (std_path_slow_path(bytes, len, index, file, line, col))
		return;
	std_path_byte_index_panic(index, reason, sizeof(reason) - 1, file, line, col);
}
#endif

/*
 * std_path_os_string_truncate is OsString::truncate: the bytes cut to a
 * length check_public_boundary allows, by Wtf8Buf::truncate on Windows,
 * which asserts the length is a code point's edge.
 */
static void
std_path_os_string_truncate(StdPathBuf *self, size_t len)
{
	if (len > self->bytes.len)
		return;
	std_path_check_public_boundary((const uint8 *) self->bytes.ptr, self->bytes.len, len, OS_STR_FILE, 582, 36);
#ifdef WIN32
	if (len <= self->bytes.len)
	{
		if (!std_path_wtf8_is_code_point_boundary((const uint8 *) self->bytes.ptr, self->bytes.len, len))
		{
			static const char text[] = "assertion failed: self.is_code_point_boundary(new_len)";

			std_panic(ALLOC_WTF8_FILE, 354, 13, text, sizeof(text) - 1);
		}
		std_vec_truncate(&self->bytes, len, 1, NULL);
	}
#else
	std_vec_truncate(&self->bytes, len, 1, NULL);
#endif
}

/*
 * std_path_components_into_iter_drop is vec::IntoIter<Component>'s drop:
 * its length as as_raw_mut_slice takes it, ExactSizeIterator::len, which
 * asserts its size_hint is exact, and then its buffer freed; a Component has
 * no drop.
 */
static void
std_path_components_into_iter_drop(StdVec *buf, size_t next)
{
	size_t		lower = buf->len - next;
	StdOptionRef upper = {true, {&lower, std_usize_Debug_fmt}};
	StdOptionRef some_lower = {true, {&lower, std_usize_Debug_fmt}};

	if (!(upper.has == some_lower.has && *(const size_t *) upper.some.value == lower))
		std_assert_failed(EXACT_SIZE_FILE, 122, 9, "==", &upper, std_option_Debug_fmt, &some_lower,
						  std_option_Debug_fmt, NULL);
	std_vec_free_Unchecked(buf, sizeof(StdComponent), _Alignof(StdComponent));
}

/* std_PathBuf_push is PathBuf::push: a relative path appended, an absolute one replacing. */
void
std_PathBuf_push(StdPathBuf *self, const char *path, size_t len)
{
	bool		need_sep = self->bytes.len > 0 &&
		!std_path_is_sep_byte(((const uint8 *) self->bytes.ptr)[self->bytes.len - 1]);
	StdComponents comps = std_path_components(self->bytes.ptr, self->bytes.len);
	StdComponents other = std_path_components(path, len);
	bool		need_clear;

	if (std_path_components_prefix_len(&comps) > 0 && std_path_components_prefix_len(&comps) == comps.path_len)
	{
		if (!comps.has_prefix)
			std_option_unwrap_failed(PATH_FILE, 1351, 29);
		if (comps.prefix.kind == STD_PREFIX_DISK)
			need_sep = false;
	}
	need_clear = std_path_is_absolute(path, len) || (HAS_PREFIXES && other.has_prefix);
	if (need_clear)
		std_path_os_string_clear(self);
	else if (std_path_components_prefix_verbatim(&comps) && len > 0)
	{
		volatile StdVec buf;
		volatile bool buf_live = false;
		volatile StdPathBuf res;
		volatile bool res_live = false;
		volatile bool iter_live = false;
		volatile size_t next = 0;
		StdComponent c;
		bool		sep = false;
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			StdVec	   *v = (StdVec *) &buf;

			if (!std_Components_next(&comps, &c))
				std_vec_new(v, sizeof(StdComponent), _Alignof(StdComponent));
			else
			{
				std_vec_with_capacity_Unchecked(v, std_max_usize(std_vec_min_non_zero_cap(sizeof(StdComponent)), 1),
												sizeof(StdComponent), _Alignof(StdComponent));
				buf_live = true;
				STD_VEC_AT(v, StdComponent, 0) = c;
				v->len = 1;
				while (std_Components_next(&comps, &c))
				{
					if (v->len == v->cap)
						std_vec_reserve_Unchecked(v, 1, sizeof(StdComponent), _Alignof(StdComponent));
					STD_VEC_AT(v, StdComponent, v->len) = c;
					v->len++;
				}
			}
			buf_live = true;
			while (std_Components_next(&other, &c))
			{
				switch (c.parsed.kind)
				{
					case STD_COMPONENT_ROOT_DIR:
						std_vec_truncate(v, 1, sizeof(StdComponent), NULL);
						std_vec_push_Unchecked(v, &c, sizeof(StdComponent), _Alignof(StdComponent));
						break;
					case STD_COMPONENT_CUR_DIR:
						break;
					case STD_COMPONENT_PARENT_DIR:
						if (v->len > 0 && STD_VEC_AT(v, StdComponent, v->len - 1).parsed.kind == STD_COMPONENT_NORMAL)
							v->len--;
						break;
					default:
						std_vec_push_Unchecked(v, &c, sizeof(StdComponent), _Alignof(StdComponent));
						break;
				}
			}
			std_PathBuf_new((StdPathBuf *) &res);
			res_live = true;
			buf_live = false;
			iter_live = true;
			while (next < v->len)
			{
				StdComponent *e = &STD_VEC_AT(v, StdComponent, next);

				next++;
				if (sep && e->parsed.kind != STD_COMPONENT_ROOT_DIR)
					std_OsString_push((StdPathBuf *) &res, MAIN_SEPARATOR_STR, 1);
				{
					StdStr		os = std_Component_as_os_str(e);

					std_OsString_push((StdPathBuf *) &res, os.ptr, os.len);
				}
				switch (e->parsed.kind)
				{
					case STD_COMPONENT_ROOT_DIR:
						sep = false;
						break;
					case STD_PREFIX_VERBATIM:
					case STD_PREFIX_VERBATIM_UNC:
					case STD_PREFIX_VERBATIM_DISK:
					case STD_PREFIX_DEVICE_NS:
					case STD_PREFIX_UNC:
					case STD_PREFIX_DISK:
						sep = e->parsed.kind != STD_PREFIX_DISK && std_path_prefix_len(&e->parsed) > 0;
						break;
					default:
						sep = true;
						break;
				}
			}
			iter_live = false;
			std_path_components_into_iter_drop(v, next);
			res_live = false;
			std_vec_free_Unchecked(&self->bytes, 1, 1);
			*self = *(StdPathBuf *) &res;
			std_unwind_pop(&unwind);
			return;
		}
		if (iter_live)
			std_path_components_into_iter_drop((StdVec *) &buf, next);
		if (res_live)
			std_vec_free_Unchecked(&((StdPathBuf *) &res)->bytes, 1, 1);
		if (buf_live)
			std_vec_free_Unchecked((StdVec *) &buf, sizeof(StdComponent), _Alignof(StdComponent));
		std_unwind_resume(&unwind);
	}
	else if (std_path_has_root(path, len))
	{
		StdComponents mine = std_path_components(self->bytes.ptr, self->bytes.len);

		std_path_os_string_truncate(self, std_path_prefix_remaining(&mine));
	}
	else if (need_sep)
		std_OsString_push(self, MAIN_SEPARATOR_STR, 1);
	std_OsString_push(self, path, len);
}

/* std_path_join is Path::join: a copy of the path with another pushed onto it, freed should the push panic. */
void
std_path_join(const char *path, size_t len, const char *other,
			  size_t other_len, StdPathBuf *result)
{
	StdUnwind	unwind;

	std_path_to_path_buf(path, len, result);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_PathBuf_push(result, other, other_len);
		std_unwind_pop(&unwind);
		return;
	}
	std_vec_free_Unchecked(&result->bytes, 1, 1);
	std_unwind_resume(&unwind);
}


/*
 * std_path_to_string_lossy is Path::to_string_lossy: the path's text,
 * borrowed when it is valid, and otherwise owned with U+FFFD in place of
 * each lone surrogate (Windows' Wtf8::to_string_lossy) or each invalid
 * sequence (Unix's String::from_utf8_lossy).
 */
void
std_path_to_string_lossy(const char *path, size_t len, StdCowStr *result)
{
#ifdef WIN32
	static const char replacement[3] = {(char) 0xEF, (char) 0xBF, (char) 0xBD};
	const uint8 *s = (const uint8 *) path;
	StdString	res;
	size_t		at;
	uint16		surrogate;
	size_t		pos;

	if (!std_path_next_surrogate(s, len, 0, &at, &surrogate))
	{
		result->tag = STD_COW_STR_BORROWED;
		result->u.borrowed = (StdStr)
		{
			path, len
		};
		return;
	}
	std_vec_with_capacity_Unchecked(&res, len, 1, 1);
	std_range_to_check(at, len, ALLOC_WTF8_FILE, 475, 45);
	std_vec_extend_from_slice_Unchecked(&res, path, at, 1, 1);
	std_vec_extend_from_slice_Unchecked(&res, replacement, 3, 1, 1);
	pos = at + 3;
	while (std_path_next_surrogate(s, len, pos, &at, &surrogate))
	{
		std_range_check(pos, at, len, ALLOC_WTF8_FILE, 481, 57);
		std_vec_extend_from_slice_Unchecked(&res, path + pos, at - pos, 1, 1);
		std_vec_extend_from_slice_Unchecked(&res, replacement, 3, 1, 1);
		pos = at + 3;
	}
	std_range_from_check(pos, len, ALLOC_WTF8_FILE, 486, 57);
	std_vec_extend_from_slice_Unchecked(&res, path + pos, len - pos, 1, 1);
	result->tag = STD_COW_STR_OWNED;
	result->u.owned = res;
#else
	std_string_from_utf8_lossy((const uint8 *) path, len, result);
#endif
}

#ifdef WIN32
/*
 * std_path_write_str_escaped is Wtf8 Debug's write_str_escaped: each char of
 * a str's escape_debug_ext written with write_char.
 */
static bool
std_path_write_str_escaped(StdFormatter *f, const char *s, size_t s_len)
{
	StdChars	chars = std_str_chars(s, s_len);
	uint32		c;

	while (std_str_Chars_next_Unchecked(&chars, &c))
	{
		char		buf[16];
		size_t		buf_len;

		if (std_char_escape_debug_ext(c, true, false, true, buf, &buf_len) == 1)
		{
			if (!std_fmt_write_char(f, c))
				return false;
		}
		else
		{
			for (size_t i = 0; i < buf_len; i++)
				if (!std_fmt_write_char(f, (uint8) buf[i]))
					return false;
		}
	}
	return true;
}
#endif

#ifndef WIN32
/* std_path_ByteStr_emit is ByteStr's Display's emit: each UTF-8 chunk's valid text, and U+FFFD for its invalid bytes. */
static bool
std_path_ByteStr_emit(const uint8 *s, size_t len, StdFormatter *f)
{
	size_t		pos = 0;
	size_t		valid;
	size_t		inspected;

	while (std_utf8_next_chunk(s + pos, len - pos, &valid, &inspected))
	{
		if (!std_fmt_write_str(f, (const char *) (s + pos), valid))
			return false;
		if (inspected != valid && !std_fmt_write_str(f, "\xEF\xBF\xBD", 3))
			return false;
		pos += inspected;
	}
	return true;
}
#endif

/*
 * std_path_Display_fmt is Path::display's Display: on Windows Wtf8's, the
 * text with U+FFFD for each lone surrogate, padded as a str is when it has
 * none; elsewhere ByteStr's, the UTF-8 chunks with U+FFFD for each invalid
 * run, truncated to the precision and padded to the width by its characters.
 * value is the path as a StdStr.
 */
bool
std_path_Display_fmt(const void *value, StdFormatter *f)
{
	const StdStr *path = (const StdStr *) value;
	const uint8 *s = (const uint8 *) path->ptr;
	size_t		len = path->len;
#ifdef WIN32
	size_t		pos = 0;
	size_t		at;
	uint16		surrogate;

	while (std_path_next_surrogate(s, len, pos, &at, &surrogate))
	{
		if (!std_fmt_write_str(f, (const char *) (s + pos), at - pos))
			return false;
		if (!std_fmt_write_char(f, 0xFFFD))
			return false;
		pos = at + 3;
	}
	if (pos == 0)
	{
		StdStr		whole = {(const char *) s, len};

		return std_str_Display_fmt(&whole, f);
	}
	return std_fmt_write_str(f, (const char *) (s + pos), len - pos);
#else
	size_t		requested_width = f->options.has_width ? f->options.width : 0;
	size_t		truncated_len = len;
	size_t		actual_width = 0;
	uint32		post_fill;
	uint16		post_padding;
	size_t		padding;

	if (requested_width == 0 && !f->options.has_precision)
		return std_path_ByteStr_emit(s, len, f);
	if (f->options.has_precision && f->options.precision == 0)
	{
		truncated_len = 0;
		actual_width = 0;
	}
	else if (f->options.has_precision)
	{
		size_t		precision = f->options.precision;
		size_t		remaining_width = precision;
		size_t		current_width = 0;
		size_t		offset = 0;
		size_t		pos = 0;
		size_t		valid;
		size_t		inspected;
		bool		cut = false;

		while (std_utf8_next_chunk(s + pos, len - pos, &valid, &inspected))
		{
			size_t		advanced = 0;
			size_t		char_offset = 0;

			while (advanced < remaining_width && char_offset < valid)
			{
				char_offset += std_str_utf8_char_width(s[pos + char_offset]);
				advanced++;
			}
			if (advanced == remaining_width)
			{
				truncated_len = offset + char_offset;
				actual_width = precision;
				cut = true;
				break;
			}
			offset += valid;
			current_width += advanced;
			remaining_width -= advanced;
			if (inspected != valid)
			{
				offset += inspected - valid;
				current_width += 1;
				remaining_width -= 1;
				if (remaining_width == 0)
				{
					truncated_len = offset;
					actual_width = precision;
					cut = true;
					break;
				}
			}
			pos += inspected;
		}
		if (!cut)
		{
			truncated_len = len;
			actual_width = current_width;
		}
	}
	else
	{
		size_t		pos = 0;
		size_t		valid;
		size_t		inspected;

		while (std_utf8_next_chunk(s + pos, len - pos, &valid, &inspected))
		{
			size_t		item = std_str_Chars_count_Unchecked((const char *) (s + pos), valid) +
				(inspected == valid ? 0 : 1);

			actual_width = std_wrapping_add_usize(actual_width, item);
			pos += inspected;
		}
	}
	padding = requested_width > actual_width ? requested_width - actual_width : 0;
	if (padding > PG_UINT16_MAX)
	{
		StdTryFromIntError tried = {STD_INT_ERROR_KIND_POS_OVERFLOW};

		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &tried,
								 std_TryFromIntError_Debug_fmt, STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP
								 "bstr" STD_SEP "mod.rs", 252, 83);
	}
	if (!std_fmt_padding(f, (uint16) padding, STD_FMT_ALIGN_LEFT, &post_fill, &post_padding))
		return false;
	if (!std_path_ByteStr_emit(s, truncated_len, f))
		return false;
	return std_fmt_post_padding(f, post_fill, post_padding);
#endif
}

/*
 * std_path_Debug_fmt is Path's Debug, its OsStr's: on Windows Wtf8's, each
 * char escaped and a lone surrogate as \u{...}; elsewhere the bytes' UTF-8
 * chunks escaped and an invalid byte as \xNN.
 */
bool
std_path_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *path = (const StdStr *) value;
	const uint8 *s = (const uint8 *) path->ptr;
	size_t		len = path->len;
#ifdef WIN32
	size_t		pos = 0;
	size_t		at;
	uint16		surrogate;

	if (!std_fmt_write_char(f, '"'))
		return false;
	while (std_path_next_surrogate(s, len, pos, &at, &surrogate))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("\\u{"), {NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}}, STD_FMT_LIT("}")};
		StdFmtArg	args[] = {{&surrogate, std_u16_LowerHex_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_range_check(pos, at, len, WTF8_FILE, 165, 53);
		if (!std_path_write_str_escaped(f, path->ptr + pos, at - pos))
			return false;
		if (!std_fmt_write_fmt_Unchecked(f, &arguments))
			return false;
		pos = at + 3;
	}
	std_range_from_check(pos, len, WTF8_FILE, 172, 83);
	if (!std_path_write_str_escaped(f, path->ptr + pos, len - pos))
		return false;
	return std_fmt_write_char(f, '"');
#else
	size_t		at = 0;
	size_t		valid;
	size_t		inspected;

	if (!std_fmt_write_char(f, '"'))
		return false;
	while (std_utf8_next_chunk(s + at, len - at, &valid, &inspected))
	{
		const char *valid_s = path->ptr + at;
		size_t		from = 0;
		StdChars	chars = std_str_chars(valid_s, valid);
		size_t		front_offset = 0;

		for (;;)
		{
			size_t		pre_len = (size_t) (chars.iter.end - chars.iter.ptr);
			size_t		i = front_offset;
			uint32		c;
			char		buf[16];
			size_t		buf_len;

			if (!std_str_Chars_next_Unchecked(&chars, &c))
				break;
			front_offset += pre_len - (size_t) (chars.iter.end - chars.iter.ptr);
			if (std_char_escape_debug_ext(c, true, false, true, buf, &buf_len) != 1)
			{
				std_str_index_range(valid_s, valid, from, i, STR_LOSSY_FILE, 132, 43);
				if (!std_fmt_write_str(f, valid_s + from, i - from))
					return false;
				for (size_t k = 0; k < buf_len; k++)
					if (!std_fmt_write_char(f, (uint8) buf[k]))
						return false;
				from = i + std_char_len_utf8(c);
			}
		}
		std_str_index_range_from(valid_s, valid, from, STR_LOSSY_FILE, 139, 35);
		if (!std_fmt_write_str(f, valid_s + from, valid - from))
			return false;
		for (size_t k = valid; k < inspected; k++)
		{
			uint8		b = s[at + k];
			StdFmtPiece pieces[] = {STD_FMT_LIT("\\x"), {NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN, .sign_aware_zero_pad = true, .has_width = true, .width = 2}}};
			StdFmtArg	args[] = {{&b, std_u8_UpperHex_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			if (!std_fmt_write_fmt_Unchecked(f, &arguments))
				return false;
		}
		at += inspected;
	}
	return std_fmt_write_char(f, '"');
#endif
}

/* std_PathBuf_Debug_fmt is PathBuf's Debug, its Path's. */
bool
std_PathBuf_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdPathBuf *self = (const StdPathBuf *) value;
	StdStr		path = {(const char *) self->bytes.ptr, self->bytes.len};

	return std_path_Debug_fmt(&path, f);
}

/*
 * std_path_rsplit_file_at_dot is rsplit_file_at_dot: a file name's part
 * before its last dot and its part after, the whole name before for `..` and
 * for a name whose only dot leads it.
 */
static void
std_path_rsplit_file_at_dot(StdStr file, bool *has_before, StdStr *before, bool *has_after, StdStr *after)
{
	bool		found = false;
	size_t		dot = 0;

	if (file.len == 2 && memcmp(file.ptr, "..", 2) == 0)
	{
		*has_before = true;
		*before = file;
		*has_after = false;
		return;
	}
	for (size_t i_end = 0, i = file.len; i > i_end;)
	{
		i--;
		if (file.ptr[i] == '.')
		{
			found = true;
			dot = i;
			break;
		}
	}
	if (!found)
	{
		*has_before = false;
		*has_after = true;
		*after = file;
		return;
	}
	if (dot == 0)
	{
		*has_before = true;
		*before = file;
		*has_after = false;
		return;
	}
	*has_before = true;
	before->ptr = file.ptr;
	before->len = dot;
	*has_after = true;
	after->ptr = file.ptr + std_wrapping_add_usize(dot, 1);
	after->len = std_wrapping_sub_usize(file.len, std_wrapping_add_usize(dot, 1));
}

/* std_path_file_stem is Path::file_stem: the file name without its extension, or none. */
bool
std_path_file_stem(const char *path, size_t len, StdStr *result)
{
	StdStr		name;
	bool		has_before;
	bool		has_after;
	StdStr		before;
	StdStr		after;

	if (!std_path_file_name(path, len, &name))
		return false;
	std_path_rsplit_file_at_dot(name, &has_before, &before, &has_after, &after);
	if (has_before)
	{
		*result = before;
		return true;
	}
	if (has_after)
	{
		*result = after;
		return true;
	}
	return false;
}

/* std_path_extension is Path::extension: the file name's part after its last dot, or none. */
bool
std_path_extension(const char *path, size_t len, StdStr *result)
{
	StdStr		name;
	bool		has_before;
	bool		has_after;
	StdStr		before;
	StdStr		after;

	if (!std_path_file_name(path, len, &name))
		return false;
	std_path_rsplit_file_at_dot(name, &has_before, &before, &has_after, &after);
	if (has_before && has_after)
	{
		*result = after;
		return true;
	}
	return false;
}

/* std_PathBuf_pop is PathBuf::pop: the path cut to its parent, or false when it has none. */
bool
std_PathBuf_pop(StdPathBuf *self)
{
	StdStr		parent;

	if (!std_path_parent(self->bytes.ptr, self->bytes.len, &parent))
		return false;
	std_path_os_string_truncate(self, parent.len);
	return true;
}

/* std_PathBuf_set_file_name is PathBuf::set_file_name: the path's file name, if any, replaced by another pushed. */
void
std_PathBuf_set_file_name(StdPathBuf *self, const char *file_name, size_t file_name_len)
{
	StdStr		name;

	if (std_path_file_name(self->bytes.ptr, self->bytes.len, &name))
		(void) std_PathBuf_pop(self);
	std_PathBuf_push(self, file_name, file_name_len);
}

/*
 * std_path_validate_extension is validate_extension: a panic naming an
 * extension that holds a path separator, in std's path.rs.
 */
static void
std_path_validate_extension(const char *extension, size_t extension_len)
{
	for (size_t i = 0, i_len = extension_len; i < i_len; i++)
	{
		if (std_path_is_sep_byte((uint8) extension[i]))
		{
			StdStr		ext = {extension, extension_len};
			StdFmtPiece pieces[] = {STD_FMT_LIT("extension cannot contain path separators: "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&ext, std_path_Debug_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			std_panic_fmt(PATH_FILE, 385, 13, &arguments);
		}
	}
}

/*
 * std_PathBuf_set_extension is PathBuf::set_extension: the path's extension
 * replaced, or the path left as it is and false when it has no file stem.
 */
bool
std_PathBuf_set_extension(StdPathBuf *self, const char *extension, size_t extension_len)
{
	StdStr		stem;
	size_t		end;

	std_path_validate_extension(extension, extension_len);
	if (!std_path_file_stem(self->bytes.ptr, self->bytes.len, &stem))
		return false;
	std_range_from_check(stem.len, stem.len, PATH_FILE, 1667, 38);
	end = std_wrapping_sub_usize((size_t) (uintptr_t) (stem.ptr + stem.len), (size_t) (uintptr_t) self->bytes.ptr);
	std_path_os_string_truncate(self, end);
	if (extension_len > 0)
	{
		std_vec_reserve_exact_Unchecked(&self->bytes, std_wrapping_add_usize(extension_len, 1), 1, 1);
		std_OsString_push(self, ".", 1);
		std_vec_extend_from_slice_Unchecked(&self->bytes, extension, extension_len, 1, 1);
	}
	return true;
}

/* std_path_with_file_name is Path::with_file_name: a copy of the path with its file name replaced. */
void
std_path_with_file_name(const char *path, size_t len, const char *file_name, size_t file_name_len,
						StdPathBuf *result)
{
	std_path_to_path_buf(path, len, result);
	std_PathBuf_set_file_name(result, file_name, file_name_len);
}

/* std_path_with_extension is Path::with_extension: a copy of the path with its extension replaced. */
void
std_path_with_extension(const char *path, size_t len, const char *extension, size_t extension_len,
						StdPathBuf *result)
{
	StdStr		previous;
	size_t		capacity;
	size_t		copied;

	if (!std_path_extension(path, len, &previous))
	{
		capacity = std_wrapping_add_usize(std_wrapping_add_usize(len, extension_len), 1);
		copied = len;
	}
	else
	{
		capacity = std_wrapping_sub_usize(std_wrapping_add_usize(len, extension_len), previous.len);
		std_range_to_check(std_wrapping_sub_usize(len, previous.len), len, PATH_FILE, 3165, 48);
		copied = std_wrapping_sub_usize(len, previous.len);
	}
	std_vec_with_capacity_Unchecked(&result->bytes, capacity, 1, 1);
#ifdef WIN32
	result->is_known_utf8 = true;
#endif
	std_vec_extend_from_slice_Unchecked(&result->bytes, path, copied, 1, 1);
#ifdef WIN32
	result->is_known_utf8 = false;
#endif
	(void) std_PathBuf_set_extension(result, extension, extension_len);
}

/* std_path_prefix_eq is Prefix's derived PartialEq: the variant, then its fields. */
static bool
std_path_prefix_eq(const StdPrefix *a, const StdPrefix *b)
{
	if (a->kind != b->kind)
		return false;
	switch (a->kind)
	{
		case STD_PREFIX_VERBATIM_DISK:
		case STD_PREFIX_DISK:
			return a->drive == b->drive;
		case STD_PREFIX_VERBATIM_UNC:
		case STD_PREFIX_UNC:
			return a->a.len == b->a.len && memcmp(a->a.ptr, b->a.ptr, a->a.len) == 0 &&
				a->b.len == b->b.len && memcmp(a->b.ptr, b->b.ptr, a->b.len) == 0;
		default:
			return a->a.len == b->a.len && memcmp(a->a.ptr, b->a.ptr, a->a.len) == 0;
	}
}

/*
 * std_Component_eq is Component's derived PartialEq: the variant, a
 * prefix's parse (PrefixComponent's own PartialEq), and a normal one's bytes.
 */
static bool
std_Component_eq(const StdComponent *a, const StdComponent *b)
{
	bool		a_prefix = a->parsed.kind != STD_COMPONENT_ROOT_DIR && a->parsed.kind != STD_COMPONENT_CUR_DIR &&
		a->parsed.kind != STD_COMPONENT_PARENT_DIR && a->parsed.kind != STD_COMPONENT_NORMAL;
	bool		b_prefix = b->parsed.kind != STD_COMPONENT_ROOT_DIR && b->parsed.kind != STD_COMPONENT_CUR_DIR &&
		b->parsed.kind != STD_COMPONENT_PARENT_DIR && b->parsed.kind != STD_COMPONENT_NORMAL;

	if (a_prefix || b_prefix)
		return a_prefix && b_prefix && std_path_prefix_eq(&a->parsed, &b->parsed);
	if (a->parsed.kind != b->parsed.kind)
		return false;
	if (a->parsed.kind == STD_COMPONENT_NORMAL)
		return a->parsed.a.len == b->parsed.a.len && memcmp(a->parsed.a.ptr, b->parsed.a.ptr, a->parsed.a.len) == 0;
	return true;
}

/*
 * std_path_PartialEq_eq is Path's PartialEq, its components' (Components'
 * PartialEq): the same bytes under the same front state and prefix form, or
 * else the components compared back to front.
 */
bool
std_path_PartialEq_eq(const char *path, size_t len, const char *other, size_t other_len)
{
	StdComponents a = std_path_components(path, len);
	StdComponents b = std_path_components(other, other_len);
	StdComponent ca;
	StdComponent cb;

	if (a.path_len == b.path_len && a.front == b.front && a.back == STD_STATE_BODY && b.back == STD_STATE_BODY &&
		std_path_components_prefix_verbatim(&a) == std_path_components_prefix_verbatim(&b) &&
		memcmp(a.path, b.path, a.path_len) == 0)
		return true;
	for (;;)
	{
		bool		has_a = std_Components_next_back(&a, &ca);
		bool		has_b;

		if (!has_a)
			return !std_Components_next_back(&b, &cb);
		has_b = std_Components_next_back(&b, &cb);
		if (!has_b || !std_Component_eq(&ca, &cb))
			return false;
	}
}
