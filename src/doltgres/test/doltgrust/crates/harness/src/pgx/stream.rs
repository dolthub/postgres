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

//! A connection's byte stream, plain or over TLS, with its read timeout and the recorder of what clients send.
//!
//! The fork's replacement for Doltgrust's crates/harness/src/pgx/stream.rs at 1cc06b328cd9, the same but for
//! `Encoding`, which converts client encodings with encoding_rs instead of Doltgrust's `types` and `cache` crates.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use std::borrow::Cow;

use pgproto::{BackendMessage, ClientEncoding, ConversionError, ErrorFields, FrameReader, FrontendMessage, Utf8};

use crate::pgx::error::Error;

/// READ_TIMEOUT bounds how long the server may take to answer a request, so that a hung or endlessly streaming
/// server fails the test instead of hanging it.
const READ_TIMEOUT: Duration = Duration::from_secs(120);

/// SharedBuffer is a byte buffer shared between a connection and its recorder.
type SharedBuffer = Arc<Mutex<Vec<u8>>>;

/// Recorder collects the raw bytes that clients send, one entry per connection, in connection order.
#[derive(Clone, Debug, Default)]
pub struct Recorder {
    connections: Arc<Mutex<Vec<SharedBuffer>>>,
}

impl Recorder {
    /// new returns an empty Recorder.
    pub fn new() -> Recorder {
        Recorder::default()
    }

    /// connection registers a new connection and returns the buffer that its sent bytes are appended to.
    fn connection(&self) -> SharedBuffer {
        let buffer = Arc::new(Mutex::new(Vec::new()));
        self.connections.lock().unwrap().push(buffer.clone());
        buffer
    }

    /// take returns the bytes sent on every connection so far, and clears the recorder.
    pub fn take(&self) -> Vec<Vec<u8>> {
        let connections = std::mem::take(&mut *self.connections.lock().unwrap());
        connections.into_iter().map(|buffer| std::mem::take(&mut *buffer.lock().unwrap())).collect()
    }
}

/// Notification is a NOTIFY payload delivered to the connection.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Notification {
    /// The process ID of the notifying backend.
    pub process_id: u32,
    /// The channel name.
    pub channel: String,
    /// The payload.
    pub payload: String,
}

/// Socket is a plaintext or TLS connection.
enum Socket {
    Plain(TcpStream),
    Tls(Box<rustls::StreamOwned<rustls::ClientConnection, TcpStream>>),
}

impl Socket {
    /// tcp returns the TCP stream under the socket.
    fn tcp(&self) -> &TcpStream {
        match self {
            Socket::Plain(tcp) => tcp,
            Socket::Tls(tls) => tls.get_ref(),
        }
    }

    /// read reads bytes from the socket.
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Socket::Plain(tcp) => tcp.read(buffer),
            Socket::Tls(tls) => tls.read(buffer),
        }
    }

    /// write_all writes bytes to the socket and flushes them.
    fn write_all(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        match self {
            Socket::Plain(tcp) => tcp.write_all(bytes),
            Socket::Tls(tls) => tls.write_all(bytes).and_then(|_| tls.flush()),
        }
    }
}

/// Stream sends and receives protocol messages over a socket, tracking the state that pgx tracks for every message.
pub(crate) struct Stream {
    socket: Socket,
    reader: FrameReader,
    write_buffer: Vec<u8>,
    recording: Option<SharedBuffer>,
    deadline: Instant,
    /// The transaction status from the most recent ReadyForQuery.
    pub(crate) tx_status: u8,
    /// The most recent value of every runtime parameter that the server reported.
    pub(crate) parameter_statuses: BTreeMap<String, String>,
    /// The notices received since they were last taken.
    pub(crate) notices: Vec<ErrorFields>,
    /// The notifications received since they were last taken.
    pub(crate) notifications: Vec<Notification>,
    /// The client encoding the server reported, which messages' text and text values are converted through.
    pub(crate) encoding: Encoding,
    /// The error of a message whose text the client encoding cannot hold, which the next flush returns.
    unsent: Option<Error>,
}

/// Encoding is the client encoding a connection speaks, which it follows from the server's `client_encoding` reports as
/// libpq does, converting as Postgres does: UTF-8 and SQL_ASCII by checking the text as UTF-8, LATIN1 by its code points,
/// and WIN1252, SJIS, and EUC_JP with encoding_rs, which converts them as Postgres' tables do but for WIN1252's five
/// unassigned bytes, which Postgres refuses. Any other encoding, MULE_INTERNAL among them, is one the client can't speak.
pub(crate) enum Encoding {
    /// UTF-8, SQL_ASCII, or a client before the server reports its encoding, whose text is checked as UTF-8.
    Utf8,
    /// LATIN1, whose bytes are the code points U+0000 to U+00FF.
    Latin1,
    /// An encoding encoding_rs converts, by its Postgres name, with the bytes Postgres' table leaves unassigned.
    Other(&'static str, &'static encoding_rs::Encoding, &'static [u8]),
}

impl Encoding {
    /// new returns the encoding of a client before the server reports one, whose text is checked as UTF-8.
    pub(crate) fn new() -> Encoding {
        Encoding::Utf8
    }

    /// follow makes the connection speak the encoding a `client_encoding` report names.
    fn follow(&mut self, name: &str) -> Result<(), Error> {
        *self = match name {
            "UTF8" | "SQL_ASCII" => Encoding::Utf8,
            "LATIN1" => Encoding::Latin1,
            "WIN1252" => Encoding::Other("WIN1252", encoding_rs::WINDOWS_1252, &[0x81, 0x8d, 0x8f, 0x90, 0x9d]),
            "SJIS" => Encoding::Other("SJIS", encoding_rs::SHIFT_JIS, &[]),
            "EUC_JP" => Encoding::Other("EUC_JP", encoding_rs::EUC_JP, &[]),
            _ => return Err(Error::Other(format!("the client cannot speak the encoding {name}"))),
        };
        Ok(())
    }

    /// text returns a text value the server sent as UTF-8.
    pub(crate) fn text(&self, bytes: &[u8]) -> Result<String, String> {
        self.decode(bytes).map_err(|err| err.message)
    }

    /// value returns a text value in the client encoding.
    pub(crate) fn value(&self, text: &[u8]) -> Result<Vec<u8>, Error> {
        let text = std::str::from_utf8(text).map_err(|_| Error::Other(invalid("UTF8", text).message))?;
        self.encode(text).map(Cow::into_owned).map_err(|err| Error::Other(err.message))
    }

    /// decode returns text in the client encoding as UTF-8.
    fn decode(&self, bytes: &[u8]) -> Result<String, ConversionError> {
        match self {
            Encoding::Utf8 => Utf8.to_server(bytes),
            Encoding::Latin1 => match bytes.iter().position(|b| *b == 0) {
                Some(at) => Err(invalid("LATIN1", &bytes[at..])),
                None => Ok(bytes.iter().map(|b| char::from(*b)).collect()),
            },
            Encoding::Other(name, encoding, unassigned) => {
                if let Some(at) = bytes.iter().position(|b| unassigned.contains(b) || *b == 0) {
                    return Err(invalid(name, &bytes[at..]));
                }
                let mut decoder = encoding.new_decoder_without_bom_handling();
                let capacity = decoder.max_utf8_buffer_length_without_replacement(bytes.len()).unwrap_or(usize::MAX);
                let mut text = String::with_capacity(capacity);
                match decoder.decode_to_string_without_replacement(bytes, &mut text, true) {
                    (encoding_rs::DecoderResult::Malformed(bad, _), read) => {
                        Err(invalid(name, &bytes[read - usize::from(bad)..]))
                    }
                    _ => Ok(text),
                }
            }
        }
    }

    /// encode returns UTF-8 text in the client encoding.
    fn encode<'s>(&self, text: &'s str) -> Result<Cow<'s, [u8]>, ConversionError> {
        match self {
            Encoding::Utf8 => Ok(Cow::Borrowed(text.as_bytes())),
            Encoding::Latin1 => match text.chars().find(|c| u32::from(*c) > 0xff) {
                Some(c) => Err(unconvertible(c, "LATIN1")),
                None => Ok(Cow::Owned(text.chars().map(|c| u32::from(c) as u8).collect())),
            },
            Encoding::Other(name, encoding, unassigned) => {
                let lacking = |c: &char| unassigned.iter().any(|b| u32::from(*b) == u32::from(*c));
                if let Some(c) = text.chars().find(lacking) {
                    return Err(unconvertible(c, name));
                }
                let mut encoder = encoding.new_encoder();
                let capacity =
                    encoder.max_buffer_length_from_utf8_without_replacement(text.len()).unwrap_or(usize::MAX);
                let mut bytes = Vec::with_capacity(capacity);
                match encoder.encode_from_utf8_to_vec_without_replacement(text, &mut bytes, true) {
                    (encoding_rs::EncoderResult::Unmappable(c), _) => Err(unconvertible(c, name)),
                    _ => Ok(Cow::Owned(bytes)),
                }
            }
        }
    }
}

impl ClientEncoding for Encoding {
    fn to_server(&mut self, bytes: &[u8]) -> Result<String, ConversionError> {
        self.decode(bytes)
    }

    fn to_client<'s>(&mut self, text: &'s str) -> Result<Cow<'s, [u8]>, ConversionError> {
        self.encode(text)
    }
}

/// hex returns bytes as Postgres lists them in a conversion error, `0x` before each and spaces between.
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("0x{b:02x}")).collect::<Vec<_>>().join(" ")
}

/// invalid returns Postgres' error for bytes that aren't text in an encoding, naming the bytes from where they go wrong,
/// at most the length of one character there, as `report_invalid_encoding` does.
fn invalid(encoding: &str, bytes: &[u8]) -> ConversionError {
    let shown = &bytes[..bytes.len().min(character_length(encoding, bytes[0]))];
    ConversionError {
        code: *b"22021",
        message: format!("invalid byte sequence for encoding \"{encoding}\": {}", hex(shown)),
    }
}

/// character_length returns the length of the character a byte starts in an encoding, as `pg_encoding_mblen` does.
fn character_length(encoding: &str, first: u8) -> usize {
    match (encoding, first) {
        ("UTF8", 0xc0..=0xdf) | ("SJIS", 0x81..=0x9f | 0xe0..=0xfc) | ("EUC_JP", 0x80..=0x8d | 0x90..=0xff) => 2,
        ("UTF8", 0xe0..=0xef) | ("EUC_JP", 0x8f) => 3,
        ("EUC_JP", 0x8e) => 2,
        ("UTF8", 0xf0..=0xf7) => 4,
        _ => 1,
    }
}

/// unconvertible returns Postgres' error for a character an encoding has no equivalent for.
fn unconvertible(c: char, encoding: &str) -> ConversionError {
    let mut buffer = [0; 4];
    ConversionError {
        code: *b"22P05",
        message: format!(
            "character with byte sequence {} in encoding \"UTF8\" has no equivalent in encoding \"{encoding}\"",
            hex(c.encode_utf8(&mut buffer).as_bytes())
        ),
    }
}

impl Stream {
    /// connect opens a socket to the address, recording sent bytes when a recorder is given.
    pub(crate) fn connect(address: &str, recorder: Option<&Recorder>) -> Result<Stream, std::io::Error> {
        let socket = TcpStream::connect(address)?;
        socket.set_nodelay(true)?;
        socket.set_write_timeout(Some(READ_TIMEOUT))?;
        Ok(Stream {
            socket: Socket::Plain(socket),
            reader: FrameReader::new(),
            write_buffer: Vec::new(),
            recording: recorder.map(Recorder::connection),
            deadline: Instant::now() + READ_TIMEOUT,
            tx_status: 0,
            parameter_statuses: BTreeMap::new(),
            notices: Vec::new(),
            notifications: Vec::new(),
            encoding: Encoding::new(),
            unsent: None,
        })
    }

    /// send queues a message to be written by the next flush, which fails instead when the client encoding cannot
    /// hold its text.
    pub(crate) fn send(&mut self, message: &FrontendMessage) {
        if let Err(err) = message.encode(&mut self.write_buffer, &mut self.encoding) {
            self.unsent.get_or_insert(Error::Other(err.message));
        }
    }

    /// flush writes every queued message.
    pub(crate) fn flush(&mut self) -> Result<(), Error> {
        if let Some(err) = self.unsent.take() {
            self.write_buffer.clear();
            return Err(err);
        }
        if self.write_buffer.is_empty() {
            return Ok(());
        }
        if let Some(recording) = &self.recording {
            recording.lock().unwrap().extend_from_slice(&self.write_buffer);
        }
        let result = self.socket.write_all(&self.write_buffer);
        self.write_buffer.clear();
        self.deadline = Instant::now() + READ_TIMEOUT;
        result?;
        Ok(())
    }

    /// write_raw writes bytes that are not a typed message, such as an SSLRequest, without queueing them.
    pub(crate) fn write_raw(&mut self, bytes: &[u8]) -> Result<(), Error> {
        if let Some(recording) = &self.recording {
            recording.lock().unwrap().extend_from_slice(bytes);
        }
        self.socket.write_all(bytes)?;
        self.deadline = Instant::now() + READ_TIMEOUT;
        Ok(())
    }

    /// read_byte reads a single unframed byte, which is how the server answers an SSLRequest.
    pub(crate) fn read_byte(&mut self) -> Result<u8, Error> {
        if let Some(byte) = self.reader.next_byte() {
            return Ok(byte);
        }
        let mut byte = [0u8; 1];
        self.socket.tcp().set_read_timeout(Some(READ_TIMEOUT))?;
        let mut read = 0;
        while read == 0 {
            read = self.socket.read(&mut byte)?;
            if read == 0 {
                return Err(Error::Other("unexpected EOF".to_string()));
            }
        }
        Ok(byte[0])
    }

    /// recv returns the next message from the server. Like pgx, it records ReadyForQuery and ParameterStatus state
    /// and collects notices and notifications before returning the message.
    pub(crate) fn recv(&mut self) -> Result<BackendMessage, Error> {
        self.recv_until(self.deadline)?
            .ok_or_else(|| Error::Other(format!("timed out after {READ_TIMEOUT:?} waiting for the server")))
    }

    /// recv_until returns the next message from the server as recv does, or None when none arrives by the deadline.
    pub(crate) fn recv_until(&mut self, deadline: Instant) -> Result<Option<BackendMessage>, Error> {
        let frame = loop {
            if let Some(frame) = self.reader.next_frame()? {
                break frame;
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(None);
            }
            self.socket.tcp().set_read_timeout(Some(remaining))?;
            let mut buffer = [0u8; 16384];
            let count = match self.socket.read(&mut buffer) {
                Ok(count) => count,
                Err(err) if matches!(err.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {
                    return Ok(None);
                }
                Err(err) => return Err(err.into()),
            };
            if count == 0 {
                return Err(Error::Other("unexpected EOF".to_string()));
            }
            self.reader.extend(&buffer[..count]);
        };
        let message = BackendMessage::decode(frame.tag, &frame.body, &mut self.encoding)?;
        match &message {
            BackendMessage::ReadyForQuery { tx_status } => self.tx_status = *tx_status,
            BackendMessage::ParameterStatus { name, value } => {
                if name == "client_encoding" {
                    self.encoding.follow(value)?;
                }
                self.parameter_statuses.insert(name.clone(), value.clone());
            }
            BackendMessage::NoticeResponse(fields) => self.notices.push(fields.clone()),
            BackendMessage::ErrorResponse(fields) if fields.severity.eq_ignore_ascii_case("FATAL") => {
                self.shutdown();
                return Err(Error::pg(fields.clone()));
            }
            BackendMessage::NotificationResponse { process_id, channel, payload } => {
                self.notifications.push(Notification {
                    process_id: *process_id,
                    channel: channel.clone(),
                    payload: payload.clone(),
                });
            }
            _ => {}
        }
        Ok(Some(message))
    }

    /// shutdown closes the socket.
    pub(crate) fn shutdown(&mut self) {
        let _ = self.socket.tcp().shutdown(std::net::Shutdown::Both);
    }

    /// start_tls performs a TLS handshake on the socket, which must be plaintext with nothing buffered.
    pub(crate) fn start_tls(
        &mut self,
        config: std::sync::Arc<rustls::ClientConfig>,
        server_name: rustls::pki_types::ServerName<'static>,
    ) -> Result<(), Error> {
        let Socket::Plain(tcp) = &self.socket else {
            return Err(Error::Other("the connection already uses TLS".to_string()));
        };
        let mut tcp = tcp.try_clone()?;
        tcp.set_read_timeout(Some(READ_TIMEOUT))?;
        let mut connection =
            rustls::ClientConnection::new(config, server_name).map_err(|e| Error::Other(e.to_string()))?;
        while connection.is_handshaking() {
            connection.complete_io(&mut tcp)?;
        }
        self.socket = Socket::Tls(Box::new(rustls::StreamOwned::new(connection, tcp)));
        Ok(())
    }
}
