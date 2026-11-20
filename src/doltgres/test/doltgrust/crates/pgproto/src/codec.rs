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

//! The frames of a stream's messages, read from the bytes as they arrive, the error of bytes that form no valid
//! message, and the conversion of messages' text between the client's encoding and the server's.

use std::borrow::Cow;
use std::fmt;

/// DecodeError is returned when bytes do not form a valid message.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodeError {
    /// A description of what was malformed.
    pub message: String,
    /// The error of a string that could not be converted from the client's encoding, when that was what was
    /// malformed.
    pub conversion: Option<ConversionError>,
}

impl DecodeError {
    /// new returns a DecodeError with the given description.
    pub fn new(message: impl Into<String>) -> DecodeError {
        DecodeError { message: message.into(), conversion: None }
    }
}

impl fmt::Display for DecodeError {
    /// fmt implements the interface Display.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for DecodeError {}

/// Frame is a single typed message read from a stream: its type byte and its body without the length prefix.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Frame {
    /// The message type byte.
    pub tag: u8,
    /// The message body, which excludes the type byte and the length.
    pub body: Vec<u8>,
}

/// PQ_SMALL_MESSAGE_LIMIT is the longest length, its own four bytes included, of a message that is never long,
/// Postgres' `PQ_SMALL_MESSAGE_LIMIT`.
pub const PQ_SMALL_MESSAGE_LIMIT: usize = 10000;
/// PQ_LARGE_MESSAGE_LIMIT is the longest length of a message that may be long, Postgres' `PQ_LARGE_MESSAGE_LIMIT`.
pub const PQ_LARGE_MESSAGE_LIMIT: usize = 0x3fff_fffe;

/// MAX_STARTUP_PACKET_LENGTH is the longest body a startup packet may have, Postgres' `MAX_STARTUP_PACKET_LENGTH`.
const MAX_STARTUP_PACKET_LENGTH: usize = 10000;

/// PQ_RECV_BUFFER_SIZE is the least room a read from the stream is given, Postgres' `PQ_RECV_BUFFER_SIZE`.
const PQ_RECV_BUFFER_SIZE: usize = 8192;

/// FrameReader accumulates bytes from a stream and splits them into frames.
#[derive(Debug, Default)]
pub struct FrameReader {
    buffer: Vec<u8>,
    start: usize,
    /// The end of the buffered bytes, after which the buffer is room for the next read.
    end: usize,
}

impl FrameReader {
    /// new returns an empty FrameReader.
    pub fn new() -> FrameReader {
        FrameReader::default()
    }

    /// is_empty reports whether no bytes are buffered.
    pub fn is_empty(&self) -> bool {
        self.start == self.end
    }

    /// extend appends bytes that were read from the stream.
    pub fn extend(&mut self, data: &[u8]) {
        self.reserve(data.len())[..data.len()].copy_from_slice(data);
        self.end += data.len();
    }

    /// unfilled returns the room after the buffered bytes for a read from the stream to fill, first moving the bytes
    /// not consumed yet to the front, as Postgres' `pq_recvbuf` does.
    pub fn unfilled(&mut self) -> &mut [u8] {
        self.reserve(PQ_RECV_BUFFER_SIZE)
    }

    /// filled marks bytes a read wrote at the start of `unfilled`'s room as buffered.
    pub fn filled(&mut self, count: usize) {
        self.end += count;
    }

    /// reserve returns the room after the buffered bytes, at least `min` bytes, first dropping the bytes already
    /// consumed.
    fn reserve(&mut self, min: usize) -> &mut [u8] {
        if self.start > 0 {
            self.buffer.copy_within(self.start..self.end, 0);
            self.end -= self.start;
            self.start = 0;
        }
        if self.buffer.len() - self.end < min {
            self.buffer.resize(self.end + min, 0);
        }
        &mut self.buffer[self.end..]
    }

    /// buffered returns the bytes that have not been consumed yet.
    pub fn buffered(&self) -> &[u8] {
        &self.buffer[self.start..self.end]
    }

    /// next_byte returns the next unframed byte, such as the answer to an SSLRequest, or None when none is buffered.
    pub fn next_byte(&mut self) -> Option<u8> {
        let byte = *self.buffered().first()?;
        self.consume(1);
        Some(byte)
    }

    /// next_frame returns the next complete typed frame, or None when more bytes are needed.
    pub fn next_frame(&mut self) -> Result<Option<Frame>, DecodeError> {
        self.next_frame_with_limit(i32::MAX as usize)
    }

    /// next_frame_with_limit is `next_frame` with the longest length, its own four bytes included, the frame may
    /// have, as Postgres' `pq_getmessage` checks it.
    pub fn next_frame_with_limit(&mut self, maxlen: usize) -> Result<Option<Frame>, DecodeError> {
        let available = self.buffered();
        if available.len() < 5 {
            return Ok(None);
        }
        let length = i32::from_be_bytes([available[1], available[2], available[3], available[4]]);
        if length < 4 || length as usize > maxlen {
            return Err(DecodeError::new("invalid message length"));
        }
        let total = 1 + length as usize;
        if available.len() < total {
            return Ok(None);
        }
        let frame = Frame { tag: available[0], body: available[5..total].to_vec() };
        self.consume(total);
        Ok(Some(frame))
    }

    /// next_untyped_frame returns the next complete frame that has no type byte, such as a StartupMessage. The
    /// returned body excludes the length.
    pub fn next_untyped_frame(&mut self) -> Result<Option<Vec<u8>>, DecodeError> {
        let available = self.buffered();
        if available.len() < 4 {
            return Ok(None);
        }
        let length = i32::from_be_bytes([available[0], available[1], available[2], available[3]]) as i64 - 4;
        if length < 4 || length > MAX_STARTUP_PACKET_LENGTH as i64 {
            return Err(DecodeError::new("invalid length of startup packet"));
        }
        let total = length as usize + 4;
        if available.len() < total {
            return Ok(None);
        }
        let body = available[4..total].to_vec();
        self.consume(total);
        Ok(Some(body))
    }

    /// consume marks the given number of buffered bytes as read.
    fn consume(&mut self, count: usize) {
        self.start += count;
        if self.start == self.end {
            self.start = 0;
            self.end = 0;
        }
    }
}

/// Reader reads protocol primitives from a message body.
pub(crate) struct Reader<'a> {
    data: &'a [u8],
    position: usize,
}

impl<'a> Reader<'a> {
    /// new returns a Reader over the given body.
    pub(crate) fn new(data: &'a [u8]) -> Reader<'a> {
        Reader { data, position: 0 }
    }

    /// remaining returns the number of unread bytes.
    pub(crate) fn remaining(&self) -> usize {
        self.data.len() - self.position
    }

    /// take returns the next count bytes.
    pub(crate) fn take(&mut self, count: usize) -> Result<&'a [u8], DecodeError> {
        if self.remaining() < count {
            return Err(DecodeError::new("insufficient data left in message"));
        }
        let bytes = &self.data[self.position..self.position + count];
        self.position += count;
        Ok(bytes)
    }

    /// rest returns all unread bytes.
    pub(crate) fn rest(&mut self) -> &'a [u8] {
        let bytes = &self.data[self.position..];
        self.position = self.data.len();
        bytes
    }

    /// u8 reads a single byte.
    pub(crate) fn u8(&mut self) -> Result<u8, DecodeError> {
        Ok(self.take(1)?[0])
    }

    /// i16 reads a big-endian 16-bit integer.
    pub(crate) fn i16(&mut self) -> Result<i16, DecodeError> {
        let bytes = self.take(2)?;
        Ok(i16::from_be_bytes([bytes[0], bytes[1]]))
    }

    /// u16 reads a big-endian unsigned 16-bit integer.
    pub(crate) fn u16(&mut self) -> Result<u16, DecodeError> {
        let bytes = self.take(2)?;
        Ok(u16::from_be_bytes([bytes[0], bytes[1]]))
    }

    /// i32 reads a big-endian 32-bit integer.
    pub(crate) fn i32(&mut self) -> Result<i32, DecodeError> {
        let bytes = self.take(4)?;
        Ok(i32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// u32 reads a big-endian unsigned 32-bit integer.
    pub(crate) fn u32(&mut self) -> Result<u32, DecodeError> {
        let bytes = self.take(4)?;
        Ok(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// cstring_bytes reads a null-terminated string's bytes.
    pub(crate) fn cstring_bytes(&mut self) -> Result<&'a [u8], DecodeError> {
        let rest = &self.data[self.position..];
        let Some(end) = rest.iter().position(|&b| b == 0) else {
            return Err(DecodeError::new("invalid string in message"));
        };
        self.position += end + 1;
        Ok(&rest[..end])
    }

    /// cstring reads a null-terminated string, which must be UTF-8.
    pub(crate) fn cstring(&mut self) -> Result<String, DecodeError> {
        String::from_utf8(self.cstring_bytes()?.to_vec()).map_err(|_| DecodeError::new("string is not valid UTF-8"))
    }

    /// text reads a null-terminated string in the client's encoding, converting it to UTF-8, Postgres'
    /// `pq_getmsgstring`.
    pub(crate) fn text(&mut self, encoding: &mut dyn ClientEncoding) -> Result<String, DecodeError> {
        let bytes = self.cstring_bytes()?;
        encoding.to_server(bytes).map_err(|err| DecodeError { message: err.message.clone(), conversion: Some(err) })
    }

    /// lossy_cstring reads a null-terminated string, replacing what is not UTF-8, as a startup packet's strings are
    /// read, which Postgres does not check.
    pub(crate) fn lossy_cstring(&mut self) -> Result<String, DecodeError> {
        Ok(String::from_utf8_lossy(self.cstring_bytes()?).into_owned())
    }

    /// nullable_bytes reads a length-prefixed byte string where a length of -1 means NULL.
    pub(crate) fn nullable_bytes(&mut self) -> Result<Option<Vec<u8>>, DecodeError> {
        let length = self.i32()?;
        if length == -1 {
            return Ok(None);
        }
        if length < 0 {
            return Err(DecodeError::new("insufficient data left in message"));
        }
        Ok(Some(self.take(length as usize)?.to_vec()))
    }
}

/// ConversionError is the error of a string that cannot be converted between the client's encoding and the server's,
/// with its SQLSTATE.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConversionError {
    /// The SQLSTATE's five characters.
    pub code: [u8; 5],
    pub message: String,
}

/// ClientEncoding converts the text of messages between the client's encoding and the server's UTF-8, Postgres'
/// `pg_client_to_server` and `pg_server_to_client` as `pq_getmsgstring` and `pq_sendstring` call them.
pub trait ClientEncoding {
    /// to_server returns text in the client's encoding as UTF-8.
    fn to_server(&mut self, bytes: &[u8]) -> Result<String, ConversionError>;

    /// to_client returns UTF-8 text in the client's encoding.
    fn to_client<'s>(&mut self, text: &'s str) -> Result<Cow<'s, [u8]>, ConversionError>;
}

/// Utf8 is the encoding of a client that speaks UTF-8, whose text needs no conversion.
pub struct Utf8;

impl ClientEncoding for Utf8 {
    fn to_server(&mut self, bytes: &[u8]) -> Result<String, ConversionError> {
        String::from_utf8(bytes.to_vec())
            .map_err(|_| ConversionError { code: *b"22021", message: "string is not valid UTF-8".into() })
    }

    fn to_client<'s>(&mut self, text: &'s str) -> Result<Cow<'s, [u8]>, ConversionError> {
        Ok(Cow::Borrowed(text.as_bytes()))
    }
}

/// Writer appends protocol primitives to a buffer.
pub(crate) struct Writer<'a> {
    buffer: &'a mut Vec<u8>,
    length_at: usize,
}

impl<'a> Writer<'a> {
    /// typed starts a message with the given type byte, reserving space for its length.
    pub(crate) fn typed(buffer: &'a mut Vec<u8>, tag: u8) -> Writer<'a> {
        buffer.push(tag);
        Writer::untyped(buffer)
    }

    /// untyped starts a message that has no type byte, reserving space for its length.
    pub(crate) fn untyped(buffer: &'a mut Vec<u8>) -> Writer<'a> {
        let length_at = buffer.len();
        buffer.extend_from_slice(&[0, 0, 0, 0]);
        Writer { buffer, length_at }
    }

    /// u8 writes a single byte.
    pub(crate) fn u8(&mut self, value: u8) -> &mut Self {
        self.buffer.push(value);
        self
    }

    /// i16 writes a big-endian 16-bit integer.
    pub(crate) fn i16(&mut self, value: i16) -> &mut Self {
        self.buffer.extend_from_slice(&value.to_be_bytes());
        self
    }

    /// u16 writes a big-endian unsigned 16-bit integer.
    pub(crate) fn u16(&mut self, value: u16) -> &mut Self {
        self.buffer.extend_from_slice(&value.to_be_bytes());
        self
    }

    /// i32 writes a big-endian 32-bit integer.
    pub(crate) fn i32(&mut self, value: i32) -> &mut Self {
        self.buffer.extend_from_slice(&value.to_be_bytes());
        self
    }

    /// u32 writes a big-endian unsigned 32-bit integer.
    pub(crate) fn u32(&mut self, value: u32) -> &mut Self {
        self.buffer.extend_from_slice(&value.to_be_bytes());
        self
    }

    /// bytes writes raw bytes.
    pub(crate) fn bytes(&mut self, value: &[u8]) -> &mut Self {
        self.buffer.extend_from_slice(value);
        self
    }

    /// cstring writes a null-terminated string.
    pub(crate) fn cstring(&mut self, value: &str) -> &mut Self {
        self.buffer.extend_from_slice(value.as_bytes());
        self.buffer.push(0);
        self
    }

    /// text writes a null-terminated string converted to the client's encoding, Postgres' `pq_sendstring`.
    pub(crate) fn text(
        &mut self,
        value: &str,
        encoding: &mut dyn ClientEncoding,
    ) -> Result<&mut Self, ConversionError> {
        self.buffer.extend_from_slice(&encoding.to_client(value)?);
        self.buffer.push(0);
        Ok(self)
    }

    /// nullable_bytes writes a length-prefixed byte string, using a length of -1 for NULL.
    pub(crate) fn nullable_bytes(&mut self, value: Option<&[u8]>) -> &mut Self {
        match value {
            Some(bytes) => {
                self.i32(bytes.len() as i32);
                self.bytes(bytes)
            }
            None => self.i32(-1),
        }
    }

    /// finish fills in the message length.
    pub(crate) fn finish(self) {
        let length = (self.buffer.len() - self.length_at) as i32;
        self.buffer[self.length_at..self.length_at + 4].copy_from_slice(&length.to_be_bytes());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn consumed_bytes_are_reclaimed_while_a_fragment_stays() {
        let frame = [b'Q', 0, 0, 0, 5, 0];
        let mut reader = FrameReader::new();
        reader.extend(&frame[..3]);
        for _ in 0..10_000 {
            reader.extend(&frame[3..]);
            reader.extend(&frame[..3]);
            assert!(reader.next_frame().unwrap().is_some());
        }
        assert!(reader.buffer.len() < 64, "the buffer holds {} bytes", reader.buffer.len());
    }
}
