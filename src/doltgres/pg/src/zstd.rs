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

//! zstd's compression, decompression, and dictionary training over the libzstd Postgres links, which the simulation
//! reaches through these wrappers alone.

use std::ptr::NonNull;
use std::sync::{PoisonError, RwLock};

use crate::ffi;

/// CompressDictionary is a zstd dictionary prepared once for compressing at a level.
pub struct CompressDictionary(NonNull<ffi::ZSTD_CDict>);

/// DecompressDictionary is a zstd dictionary prepared once for decompressing.
pub struct DecompressDictionary(NonNull<ffi::ZSTD_DDict>);

// SAFETY: zstd.h says a "ZSTD_CDict can be created once and shared by multiple threads concurrently, since its usage
// is read-only", so moving one to another thread, or using one from several at once, is sound. Tested by
// tests::a_dictionary_round_trips_what_it_compresses.
unsafe impl Send for CompressDictionary {}

// SAFETY: as for `Send`. Tested by tests::a_dictionary_round_trips_what_it_compresses.
unsafe impl Sync for CompressDictionary {}

// SAFETY: libzstd takes a DDict only as `const ZSTD_DDict *`, which zstd.h says is "just referenced" by the contexts
// that decompress with it, and decompression copies the DDict's parameters into its own context
// (`ZSTD_copyDDictParameters`) rather than writing the DDict, so its use is read-only, as a CDict's is, and moving one
// to another thread, or using one from several at once, is sound. Tested by
// tests::a_dictionary_round_trips_what_it_compresses.
unsafe impl Send for DecompressDictionary {}

// SAFETY: as for `Send`. Tested by tests::a_dictionary_round_trips_what_it_compresses.
unsafe impl Sync for DecompressDictionary {}

impl CompressDictionary {
    /// new prepares a dictionary's bytes for compressing at the level, or returns None when libzstd can't allocate
    /// them.
    pub fn new(bytes: &[u8], level: i32) -> Option<CompressDictionary> {
        // SAFETY: libzstd copies the bytes, which live through the call. Tested by
        // tests::a_dictionary_round_trips_what_it_compresses.
        NonNull::new(unsafe { ffi::ZSTD_createCDict(bytes.as_ptr().cast(), bytes.len(), level) })
            .map(CompressDictionary)
    }
}

impl DecompressDictionary {
    /// new prepares a dictionary's bytes for decompressing, or returns None when libzstd can't allocate them.
    pub fn new(bytes: &[u8]) -> Option<DecompressDictionary> {
        // SAFETY: libzstd copies the bytes, which live through the call. Tested by
        // tests::a_dictionary_round_trips_what_it_compresses.
        NonNull::new(unsafe { ffi::ZSTD_createDDict(bytes.as_ptr().cast(), bytes.len()) }).map(DecompressDictionary)
    }
}

impl Drop for CompressDictionary {
    fn drop(&mut self) {
        // SAFETY: the dictionary is libzstd's, made by `new`, and freed only here. Tested by
        // tests::a_dictionary_round_trips_what_it_compresses.
        unsafe { ffi::ZSTD_freeCDict(self.0.as_ptr()) };
    }
}

impl Drop for DecompressDictionary {
    fn drop(&mut self) {
        // SAFETY: the dictionary is libzstd's, made by `new`, and freed only here. Tested by
        // tests::a_dictionary_round_trips_what_it_compresses.
        unsafe { ffi::ZSTD_freeDDict(self.0.as_ptr()) };
    }
}

/// Context is a libzstd compression or decompression context, which keeps its memory from one call to the next.
struct Context<T>(NonNull<T>);

// SAFETY: the process's contexts sit in statics, which need `Sync`; a libzstd context isn't tied to a thread, and its
// lock keeps it to one call at a time. Tested by tests::compression_round_trips.
unsafe impl<T> Send for Context<T> {}

// SAFETY: as for `Send`. Tested by tests::compression_round_trips.
unsafe impl<T> Sync for Context<T> {}

/// COMPRESSOR is the process's compression context, made at its first use.
static COMPRESSOR: RwLock<Option<Context<ffi::ZSTD_CCtx>>> = RwLock::new(None);

/// DECOMPRESSOR is the process's decompression context, made at its first use.
static DECOMPRESSOR: RwLock<Option<Context<ffi::ZSTD_DCtx>>> = RwLock::new(None);

/// compress returns the bytes compressed as one frame that records their length, at the level or with the
/// dictionary, which carries its own, or None when libzstd can't allocate its context.
pub fn compress(bytes: &[u8], level: i32, dictionary: Option<&CompressDictionary>) -> Option<Vec<u8>> {
    let mut context = COMPRESSOR.write().unwrap_or_else(PoisonError::into_inner);
    if context.is_none() {
        // SAFETY: the call takes no argument. Tested by tests::compression_round_trips.
        *context = Some(Context(NonNull::new(unsafe { ffi::ZSTD_createCCtx() })?));
    }
    let cctx = context.as_ref()?.0.as_ptr();
    // SAFETY: the call reads only its argument. Tested by tests::compression_round_trips.
    let bound = unsafe { ffi::ZSTD_compressBound(bytes.len()) };
    let mut out: Vec<u8> = Vec::with_capacity(bound);
    // SAFETY: the context is libzstd's and the lock keeps it to this call; `out` has room for `bound` bytes, which
    // libzstd writes no further than, and `bytes` and the dictionary live through the call. Tested by
    // tests::compression_round_trips; tests::a_dictionary_round_trips_what_it_compresses.
    let written = unsafe {
        match dictionary {
            Some(dictionary) => ffi::ZSTD_compress_usingCDict(
                cctx,
                out.as_mut_ptr().cast(),
                bound,
                bytes.as_ptr().cast(),
                bytes.len(),
                dictionary.0.as_ptr(),
            ),
            None => {
                ffi::ZSTD_compressCCtx(cctx, out.as_mut_ptr().cast(), bound, bytes.as_ptr().cast(), bytes.len(), level)
            }
        }
    };
    // SAFETY: the call reads only its argument. Tested by tests::compression_round_trips.
    if unsafe { ffi::ZSTD_isError(written) } != 0 {
        return None;
    }
    // SAFETY: libzstd wrote `written` bytes, no more than the capacity, into `out`. Tested by
    // tests::compression_round_trips.
    unsafe { out.set_len(written) };
    Some(out)
}

/// frame_size returns the length a frame records of its bytes, or None when it records none or is no frame.
pub fn frame_size(frame: &[u8]) -> Option<u64> {
    // SAFETY: the call reads `frame`, which lives through it, no further than its length. Tested by
    // tests::damaged_frames_record_no_size.
    let size = unsafe { ffi::ZSTD_getFrameContentSize(frame.as_ptr().cast(), frame.len()) };
    (size < ffi::ZSTD_CONTENTSIZE_ERROR as u64).then_some(size)
}

/// Failed is why a frame failed to decompress.
#[derive(Debug, PartialEq, Eq)]
pub enum Failed {
    /// The frame's bytes are damaged, or are not the `size` bytes it was asked for.
    Damaged,
    /// libzstd couldn't allocate its decompression context.
    OutOfMemory,
}

/// decompress returns a frame's `size` bytes, decompressed with the dictionary if it names one.
pub fn decompress(frame: &[u8], size: usize, dictionary: Option<&DecompressDictionary>) -> Result<Vec<u8>, Failed> {
    let mut context = DECOMPRESSOR.write().unwrap_or_else(PoisonError::into_inner);
    if context.is_none() {
        // SAFETY: the call takes no argument. Tested by tests::compression_round_trips.
        let made = NonNull::new(unsafe { ffi::ZSTD_createDCtx() }).ok_or(Failed::OutOfMemory)?;
        *context = Some(Context(made));
    }
    let dctx = context.as_ref().ok_or(Failed::OutOfMemory)?.0.as_ptr();
    let mut out: Vec<u8> = Vec::with_capacity(size);
    // SAFETY: the context is libzstd's and the lock keeps it to this call; `out` has room for `size` bytes, which
    // libzstd writes no further than, and `frame` and the dictionary live through the call. Tested by
    // tests::compression_round_trips; tests::damaged_frames_fail_as_damaged.
    let written = unsafe {
        match dictionary {
            Some(dictionary) => ffi::ZSTD_decompress_usingDDict(
                dctx,
                out.as_mut_ptr().cast(),
                size,
                frame.as_ptr().cast(),
                frame.len(),
                dictionary.0.as_ptr(),
            ),
            None => ffi::ZSTD_decompressDCtx(dctx, out.as_mut_ptr().cast(), size, frame.as_ptr().cast(), frame.len()),
        }
    };
    // SAFETY: the call reads only its argument. Tested by tests::damaged_frames_fail_as_damaged.
    if unsafe { ffi::ZSTD_isError(written) } != 0 || written != size {
        return Err(Failed::Damaged);
    }
    // SAFETY: libzstd wrote `written` bytes, the capacity, into `out`. Tested by tests::compression_round_trips.
    unsafe { out.set_len(written) };
    Ok(out)
}

/// train returns a dictionary of at most `capacity` bytes trained on the samples, or None when libzstd can't train
/// one on them.
pub fn train(samples: &[&[u8]], capacity: usize) -> Option<Vec<u8>> {
    let joined = samples.concat();
    let sizes: Vec<usize> = samples.iter().map(|sample| sample.len()).collect();
    let count = u32::try_from(samples.len()).ok()?;
    let mut out: Vec<u8> = Vec::with_capacity(capacity);
    // SAFETY: `out` has room for `capacity` bytes, which libzstd writes no further than, and `joined` holds the
    // samples whose `count` lengths `sizes` gives, all living through the call. Tested by
    // tests::a_dictionary_round_trips_what_it_compresses.
    let written = unsafe {
        ffi::ZDICT_trainFromBuffer(out.as_mut_ptr().cast(), capacity, joined.as_ptr().cast(), sizes.as_ptr(), count)
    };
    // SAFETY: the call reads only its argument. Tested by tests::too_few_samples_train_nothing.
    if unsafe { ffi::ZDICT_isError(written) } != 0 {
        return None;
    }
    // SAFETY: libzstd wrote `written` bytes, no more than the capacity, into `out`. Tested by
    // tests::a_dictionary_round_trips_what_it_compresses.
    unsafe { out.set_len(written) };
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// samples returns leaves alike enough for a dictionary to help, each naming its number.
    fn samples() -> Vec<Vec<u8>> {
        (0..256)
            .map(|n| format!("{{\"id\": {n}, \"name\": \"row {n}\", \"kind\": \"versioned\"}}").repeat(8).into_bytes())
            .collect()
    }

    /// Bytes compress to fewer and decompress to the same, through one context the calls share.
    #[test]
    fn compression_round_trips() {
        let bytes = b"versioned ".repeat(100);
        let frame = compress(&bytes, 3, None).expect("compresses");
        assert!(frame.len() < bytes.len());
        assert_eq!(frame_size(&frame), Some(1000));
        assert_eq!(decompress(&frame, 1000, None).as_deref(), Ok(&bytes[..]));
        let empty = compress(b"", 3, None).expect("compresses");
        assert_eq!(decompress(&empty, 0, None).as_deref(), Ok(&b""[..]));
    }

    /// Bytes that are no frame, or are cut before its header ends, record no size.
    #[test]
    fn damaged_frames_record_no_size() {
        let frame = compress(&b"versioned ".repeat(100), 3, None).expect("compresses");
        assert_eq!(frame_size(b"not a frame"), None);
        assert_eq!(frame_size(&frame[..3]), None);
        assert_eq!(frame_size(&[]), None);
    }

    /// A frame cut short, one asked for another size than it holds, and one whose bytes flipped fail as damaged.
    #[test]
    fn damaged_frames_fail_as_damaged() {
        let frame = compress(&b"versioned ".repeat(100), 3, None).expect("compresses");
        assert_eq!(decompress(&frame[..frame.len() - 1], 1000, None), Err(Failed::Damaged));
        assert_eq!(decompress(&frame, 999, None), Err(Failed::Damaged));
        let mut flipped = frame.clone();
        let last = flipped.len() - 1;
        flipped[last] ^= 0xff;
        assert_eq!(decompress(&flipped, 1000, None), Err(Failed::Damaged));
    }

    /// A trained dictionary compresses a sample to fewer bytes than none does, and reads back only with itself.
    #[test]
    fn a_dictionary_round_trips_what_it_compresses() {
        let samples = samples();
        let trained = train(&samples.iter().map(Vec::as_slice).collect::<Vec<_>>(), 16 << 10).expect("trains");
        let (compressing, decompressing) = (
            CompressDictionary::new(&trained, 3).expect("prepares"),
            DecompressDictionary::new(&trained).expect("prepares"),
        );
        let leaf = &samples[7];
        let frame = compress(leaf, 3, Some(&compressing)).expect("compresses");
        assert!(frame.len() < compress(leaf, 3, None).expect("compresses").len());
        assert_eq!(decompress(&frame, leaf.len(), Some(&decompressing)).as_deref(), Ok(&leaf[..]));
        assert_eq!(decompress(&frame, leaf.len(), None), Err(Failed::Damaged));
    }

    /// Too few samples train no dictionary.
    #[test]
    fn too_few_samples_train_nothing() {
        assert_eq!(train(&[b"one sample"], 16 << 10), None);
    }
}
