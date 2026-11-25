// Copyright 2026 Dolthub, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Rust's own std::fs for the C tests (`std/test_fs.h`, the port text's section 13): each function of std::fs the
//! crates' tests call, exported under section 2's std name, its errors as the C's `io::Error`.

// Tests reach files directly, as the Rust tests do with std::fs; simc is a test crate the server never links.
#![allow(clippy::disallowed_methods, clippy::disallowed_types)]

use std::ffi::{c_char, c_void};
use std::fs::{File, Metadata, OpenOptions};
use std::io::Write;

use crate::api::{handed, taken};
use crate::c;
use crate::entry::entered;
use crate::ffi;

/// answered writes an std::fs call's answer to the C's places, and returns whether it succeeded.
///
/// # Safety
///
/// `result` and `error` are places the C passed for the answer.
unsafe fn answered<T>(answer: std::io::Result<T>, result: *mut T, error: *mut ffi::StdIoError) -> bool {
    match answer {
        // SAFETY: the caller's contract, a place for the value. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        Ok(value) => unsafe { result.write(value) },
        // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error.
        Err(failure) => return unsafe { failed(Err(failure), error) },
    }
    true
}

/// failed writes an std::fs call's error to the C's place, and returns whether the call succeeded.
///
/// # Safety
///
/// `error` is a place the C passed for the error.
unsafe fn failed(answer: std::io::Result<()>, error: *mut ffi::StdIoError) -> bool {
    let Err(failure) = answer else { return true };
    let made = c::io_error(&failure);
    // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error.
    unsafe { error.write(made) };
    false
}

/// std_fs_read is `fs::read`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `result` and `error` places for the answer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_read(
    path: *const c_char,
    path_len: usize,
    result: *mut ffi::StdVec,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { answered(std::fs::read(c::path(path, path_len)).map(|bytes| c::vec(&bytes)), result, error) }
    })
}

/// std_fs_write is `fs::write`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, `contents` and `contents_len` the bytes to write, and `error` a place for
/// the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_write(
    path: *const c_char,
    path_len: usize,
    contents: *const u8,
    contents_len: usize,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { failed(std::fs::write(c::path(path, path_len), c::slice(contents, contents_len)), error) }
    })
}

/// std_fs_remove_file is `fs::remove_file`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_remove_file(path: *const c_char, path_len: usize, error: *mut ffi::StdIoError) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { failed(std::fs::remove_file(c::path(path, path_len)), error) }
    })
}

/// std_fs_rename is `fs::rename`.
///
/// # Safety
///
/// `from`, `from_len`, `to`, and `to_len` are two paths' bytes, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_rename(
    from: *const c_char,
    from_len: usize,
    to: *const c_char,
    to_len: usize,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_lists_a_directorys_names.
        unsafe { failed(std::fs::rename(c::path(from, from_len), c::path(to, to_len)), error) }
    })
}

/// std_fs_create_dir_all is `fs::create_dir_all`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_create_dir_all(
    path: *const c_char,
    path_len: usize,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_makes_a_directory_with_its_parents.
        unsafe { failed(std::fs::create_dir_all(c::path(path, path_len)), error) }
    })
}

/// std_fs_remove_dir_all is `fs::remove_dir_all`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_remove_dir_all(
    path: *const c_char,
    path_len: usize,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_removes_a_directory_with_its_contents.
        unsafe { failed(std::fs::remove_dir_all(c::path(path, path_len)), error) }
    })
}

/// std_fs_read_dir_names is `fs::read_dir` with each entry's `file_name().to_string_lossy()`: the names of a
/// directory's entries, as a test lists them, or the error of the directory's read or of an entry's, with whether an
/// entry's read failed.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `result`, `entry`, and `error` places for the answer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_read_dir_names(
    path: *const c_char,
    path_len: usize,
    result: *mut ffi::StdVec,
    entry: *mut bool,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_lists_a_directorys_names.
        let read = std::fs::read_dir(unsafe { c::path(path, path_len) });
        let names = read.map(|entries| {
            let named = entries.map(|found| found.map(|found| found.file_name().to_string_lossy().into_owned()));
            named.collect::<std::io::Result<Vec<String>>>()
        });
        // SAFETY: the caller's contract, a place for whether an entry failed. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_lists_a_directorys_names.
        unsafe { entry.write(matches!(names, Ok(Err(_)))) };
        // SAFETY: the caller's contract, places for the names and the error. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_lists_a_directorys_names.
        unsafe { answered(names.and_then(|names| names).map(|names| c::strings(&names)), result, error) }
    })
}

/// std_fs_metadata is `fs::metadata`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `result` and `error` places for the answer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_metadata(
    path: *const c_char,
    path_len: usize,
    result: *mut ffi::StdFsMetadata,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        let answer = std::fs::metadata(
            // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
            unsafe { c::path(path, path_len) },
        );
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { answered(answer.map(|metadata| ffi::StdFsMetadata { f0: handed(metadata) }), result, error) }
    })
}

/// std_fs_Metadata_len is `Metadata::len`.
///
/// # Safety
///
/// `metadata` is metadata `std_fs_metadata` made, not yet dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_Metadata_len(metadata: *const ffi::StdFsMetadata) -> u64 {
    // SAFETY: the caller's contract, a box of a `Metadata`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
    entered!(unsafe { &*(*metadata).f0.cast::<Metadata>() }.len())
}

/// std_fs_Metadata_drop is Metadata's drop.
///
/// # Safety
///
/// `metadata` is metadata `std_fs_metadata` made, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_Metadata_drop(metadata: *mut c_void) {
    // SAFETY: the caller's contract, a box of a `Metadata`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
    entered!(drop(unsafe { taken::<Metadata>((*metadata.cast::<ffi::StdFsMetadata>()).f0) }))
}

/// std_fs_OpenOptions_new is `OpenOptions::new`.
#[unsafe(no_mangle)]
pub extern "C" fn std_fs_OpenOptions_new() -> ffi::StdFsOpenOptions {
    entered!(ffi::StdFsOpenOptions { f0: handed(OpenOptions::new()) })
}

/// std_fs_OpenOptions_append is `OpenOptions::append`.
///
/// # Safety
///
/// `options` are options `std_fs_OpenOptions_new` made, not yet dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_OpenOptions_append(
    options: *mut ffi::StdFsOpenOptions,
    append: bool,
) -> *mut ffi::StdFsOpenOptions {
    entered!({
        // SAFETY: the caller's contract, a box of an `OpenOptions`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { &mut *(*options).f0.cast::<OpenOptions>() }.append(append);
        options
    })
}

/// std_fs_OpenOptions_open is `OpenOptions::open`.
///
/// # Safety
///
/// `options` are options `std_fs_OpenOptions_new` made, not yet dropped; `path` and `path_len` a path's bytes; and
/// `result` and `error` places for the answer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_OpenOptions_open(
    options: *const ffi::StdFsOpenOptions,
    path: *const c_char,
    path_len: usize,
    result: *mut ffi::StdFsFile,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe {
            let answer = (*(*options).f0.cast::<OpenOptions>()).open(c::path(path, path_len));
            answered(answer.map(|file| ffi::StdFsFile { f0: handed(file) }), result, error)
        }
    })
}

/// std_fs_OpenOptions_drop is OpenOptions' drop.
///
/// # Safety
///
/// `options` are options `std_fs_OpenOptions_new` made, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_OpenOptions_drop(options: *mut c_void) {
    // SAFETY: the caller's contract, a box of an `OpenOptions`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
    entered!(drop(unsafe { taken::<OpenOptions>((*options.cast::<ffi::StdFsOpenOptions>()).f0) }))
}

/// std_fs_File_Write_write_all is File's `Write::write_all`.
///
/// # Safety
///
/// `file` is a file `std_fs_OpenOptions_open` opened, not yet dropped; `buf` and `buf_len` the bytes to write; and
/// `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_File_Write_write_all(
    file: *mut ffi::StdFsFile,
    buf: *const u8,
    buf_len: usize,
    error: *mut ffi::StdIoError,
) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
        unsafe { failed((*(*file).f0.cast::<File>()).write_all(c::slice(buf, buf_len)), error) }
    })
}

/// std_fs_File_drop is File's drop, which closes it.
///
/// # Safety
///
/// `file` is a file `std_fs_OpenOptions_open` opened, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_File_drop(file: *mut c_void) {
    // SAFETY: the caller's contract, a box of a `File`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
    entered!(drop(unsafe { taken::<File>((*file.cast::<ffi::StdFsFile>()).f0) }))
}

/// std_path_Path_exists is `Path::exists`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_path_Path_exists(path: *const c_char, path_len: usize) -> bool {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file.
    entered!(unsafe { c::path(path, path_len) }.exists())
}

/// std_fs_create_dir is `fs::create_dir`.
///
/// # Safety
///
/// `path` and `path_len` are a path's bytes, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn std_fs_create_dir(path: *const c_char, path_len: usize, error: *mut ffi::StdIoError) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands.
        unsafe { failed(std::fs::create_dir(c::path(path, path_len)), error) }
    })
}
