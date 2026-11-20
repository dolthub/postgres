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

use std::time::Duration;

use harness::server::{Server, Target};
use harness::wire::RawConnection;
use pgproto::BackendMessage;

/// query returns a Query message of the bytes, which need not be UTF-8.
fn query(text: &[u8]) -> Vec<u8> {
    let mut message = vec![b'Q'];
    message.extend_from_slice(&(text.len() as u32 + 5).to_be_bytes());
    message.extend_from_slice(text);
    message.push(0);
    message
}

/// run sends a Query message and returns the codes of the errors it met and the transaction status after it.
fn run(conn: &mut RawConnection, text: &[u8]) -> (Vec<(String, String)>, u8) {
    answer(conn, &query(text))
}

/// answer sends bytes and returns the codes and messages of the errors the server answers with and the transaction
/// status of its ReadyForQuery.
fn answer(conn: &mut RawConnection, bytes: &[u8]) -> (Vec<(String, String)>, u8) {
    conn.send_bytes(bytes).unwrap();
    let mut errors = Vec::new();
    loop {
        match conn.receive(Duration::from_secs(30)).unwrap().expect("the server answers") {
            BackendMessage::ErrorResponse(fields) => errors.push((fields.code, fields.message)),
            BackendMessage::ReadyForQuery { tx_status } => return (errors, tx_status),
            _ => {}
        }
    }
}

#[test]
fn queries_that_are_not_utf8_fail_and_the_session_goes_on() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = RawConnection::connect(server.port, &[("user", "postgres"), ("database", "postgres")]).unwrap();
    assert_eq!(run(&mut conn, b"BEGIN"), (Vec::new(), b'T'));
    let invalid = vec![("22021".to_string(), "invalid byte sequence for encoding \"UTF8\": 0xff".to_string())];
    assert_eq!(run(&mut conn, b"SET application_name = 'a\xffb'"), (invalid, b'E'));
    assert_eq!(run(&mut conn, b"ROLLBACK"), (Vec::new(), b'I'));
}

#[cfg(unix)]
#[test]
fn fast_shutdowns_end_sessions_as_postgres_does() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = RawConnection::connect(server.port, &[("user", "postgres"), ("database", "postgres")]).unwrap();
    assert_eq!(run(&mut conn, b"BEGIN"), (Vec::new(), b'T'));
    std::process::Command::new("kill").args(["-INT", &server.pid().to_string()]).status().unwrap();
    let Some(BackendMessage::ErrorResponse(fields)) = conn.receive(Duration::from_secs(30)).unwrap() else {
        panic!("expected an error");
    };
    assert_eq!(
        (fields.severity.as_str(), fields.code.as_str(), fields.message.as_str()),
        ("FATAL", "57P01", "terminating connection due to administrator command")
    );
    while server.is_running() {
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[cfg(unix)]
#[test]
fn smart_shutdowns_refuse_new_sessions_and_wait_for_running_ones() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    std::process::Command::new("kill").args(["-TERM", &server.pid().to_string()]).status().unwrap();
    let refused = loop {
        match RawConnection::connect(server.port, &parameters) {
            Ok(_) => std::thread::sleep(Duration::from_millis(20)),
            Err(message) => break message,
        }
    };
    assert_eq!(refused, "the database system is shutting down");
    assert_eq!(run(&mut conn, b"BEGIN"), (Vec::new(), b'T'));
    assert!(server.is_running());
    drop(conn);
    while server.is_running() {
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[cfg(unix)]
#[test]
fn fast_shutdowns_end_a_smart_shutdown_that_waits_for_sessions() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    assert_eq!(run(&mut conn, b"BEGIN"), (Vec::new(), b'T'));
    for signal in ["-TERM", "-TERM"] {
        std::process::Command::new("kill").args([signal, &server.pid().to_string()]).status().unwrap();
    }
    while RawConnection::connect(server.port, &parameters).is_ok() {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(run(&mut conn, b"SELECT 1"), (Vec::new(), b'T'));
    assert!(server.is_running());
    std::process::Command::new("kill").args(["-INT", &server.pid().to_string()]).status().unwrap();
    let Some(BackendMessage::ErrorResponse(fields)) = conn.receive(Duration::from_secs(30)).unwrap() else {
        panic!("expected an error");
    };
    assert_eq!(
        (fields.severity.as_str(), fields.code.as_str(), fields.message.as_str()),
        ("FATAL", "57P01", "terminating connection due to administrator command")
    );
    while server.is_running() {
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// ENCODED are text in client encodings, its bytes in the encoding, and the hexadecimal of its UTF-8, as PG18 sends and
/// reads them.
const ENCODED: &[(&str, &[u8], &str)] = &[
    ("SJIS", b"\x95\x5c\x83\x5c", "e8a1a8e382bd"),
    ("EUC_JP", b"\xc6\xfc\xcb\xdc", "e697a5e69cac"),
    ("BIG5", b"\xb3\x5c\xa5\x5c", "e8a8b1e58a9f"),
    ("GB18030", b"\xd6\xd0\x95\x32\x82\x36", "e4b8adf0a08080"),
    ("EUC_KR", b"\xc7\xd1\xb1\xb9", "ed959ceab5ad"),
];

/// message returns a typed message of a body.
fn message(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut message = vec![tag];
    message.extend_from_slice(&(body.len() as u32 + 4).to_be_bytes());
    message.extend_from_slice(body);
    message
}

/// frames sends bytes and returns the bodies of the messages of each type the server answers with, up to its
/// ReadyForQuery.
fn frames(conn: &mut RawConnection, bytes: &[u8]) -> Vec<(u8, Vec<u8>)> {
    conn.send_bytes(bytes).unwrap();
    let mut frames = Vec::new();
    loop {
        let frame = conn.receive_frame(Duration::from_secs(30)).unwrap().expect("the server answers");
        if frame.tag == b'Z' {
            return frames;
        }
        frames.push((frame.tag, frame.body));
    }
}

/// sent returns whether a message of the type holds the bytes.
fn sent(frames: &[(u8, Vec<u8>)], tag: u8, bytes: &[u8]) -> bool {
    frames.iter().any(|(t, body)| *t == tag && body.windows(bytes.len()).any(|window| window == bytes))
}

#[test]
fn client_encodings_carry_the_bytes_postgres_sends_and_reads() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    for &(encoding, text, utf8) in ENCODED {
        let parameters = [("user", "postgres"), ("database", "postgres"), ("client_encoding", encoding)];
        let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
        let utf8_text = format!("\\x{utf8}");
        let literal = [b"SELECT convert_to('".as_slice(), text, b"', 'UTF8')::text"].concat();
        let answer = frames(&mut conn, &query(&literal));
        assert!(sent(&answer, b'D', utf8_text.as_bytes()), "{encoding}: a query literal: {answer:?}");
        let named = [format!("SELECT convert_from('{utf8_text}', 'UTF8') AS \"").as_bytes(), text, b"\""].concat();
        let answer = frames(&mut conn, &query(&named));
        assert!(sent(&answer, b'T', &[text, b"\0"].concat()), "{encoding}: a column name: {answer:?}");
        assert!(sent(&answer, b'D', &[&(text.len() as u32).to_be_bytes(), text].concat()), "{encoding}: row text: {answer:?}");
        let failing = format!("SELECT convert_from('{utf8_text}', 'UTF8')::int");
        let answer = frames(&mut conn, &query(failing.as_bytes()));
        let error = [b"Minvalid input syntax for type integer: \"".as_slice(), text, b"\"\0"].concat();
        assert!(sent(&answer, b'E', &error), "{encoding}: an error message: {answer:?}");
        let parse = message(b'P', b"\0SELECT convert_to($1::text, 'UTF8')::text\0\0\0");
        let bind = [b"\0\0\0\0\0\x01".as_slice(), &(text.len() as u32).to_be_bytes(), text, b"\0\0"].concat();
        let extended = [parse, message(b'B', &bind), message(b'E', b"\0\0\0\0\0"), message(b'S', b"")].concat();
        let answer = frames(&mut conn, &extended);
        assert!(sent(&answer, b'D', utf8_text.as_bytes()), "{encoding}: a parameter: {answer:?}");
    }
}

#[test]
#[ignore = "xml's input function is missing until the xml type comes in Stage 3"]
fn xml_sent_in_binary_declares_the_client_encoding() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    for &(encoding, text, _) in ENCODED {
        let parameters = [("user", "postgres"), ("database", "postgres"), ("client_encoding", encoding)];
        let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
        let parse = message(b'P', &[b"\0SELECT '<a>".as_slice(), text, b"</a>'::xml\0\0\0"].concat());
        let binary = message(b'B', b"\0\0\0\0\0\0\0\x01\0\x01");
        let extended = [parse, binary, message(b'E', b"\0\0\0\0\0"), message(b'S', b"")].concat();
        let answer = frames(&mut conn, &extended);
        let declared = [format!("<?xml version=\"1.0\" encoding=\"{encoding}\"?><a>").as_bytes(), text, b"</a>"].concat();
        assert!(sent(&answer, b'D', &declared), "{encoding}: xml's binary send: {answer:?}");
    }
}

/// function_call returns a FunctionCall message of a function's OID, argument formats, arguments, and result format.
fn function_call(fid: u32, formats: &[u16], args: &[Option<&[u8]>], rformat: u16) -> Vec<u8> {
    let mut body = fid.to_be_bytes().to_vec();
    body.extend_from_slice(&(formats.len() as u16).to_be_bytes());
    formats.iter().for_each(|format| body.extend_from_slice(&format.to_be_bytes()));
    body.extend_from_slice(&(args.len() as u16).to_be_bytes());
    for arg in args {
        match arg {
            None => body.extend_from_slice(&(-1i32).to_be_bytes()),
            Some(arg) => {
                body.extend_from_slice(&(arg.len() as u32).to_be_bytes());
                body.extend_from_slice(arg);
            }
        }
    }
    body.extend_from_slice(&rformat.to_be_bytes());
    message(b'F', &body)
}

/// Answer is a function's result, None for NULL, or the error's code and message.
type Answer = Result<Option<Vec<u8>>, (String, String)>;

/// call sends a FunctionCall message and returns the function's answer and the transaction status after it.
fn call(conn: &mut RawConnection, bytes: &[u8]) -> (Answer, u8) {
    conn.send_bytes(bytes).unwrap();
    let mut answer = Err((String::new(), String::new()));
    loop {
        match conn.receive(Duration::from_secs(30)).unwrap().expect("the server answers") {
            BackendMessage::FunctionCallResponse { result } => answer = Ok(result),
            BackendMessage::ErrorResponse(fields) => answer = Err((fields.code, fields.message)),
            BackendMessage::ReadyForQuery { tx_status } => return (answer, tx_status),
            _ => {}
        }
    }
}

/// oid returns the OID a query's one value names.
fn oid(conn: &mut RawConnection, text: &str) -> u32 {
    let answer = frames(conn, &query(text.as_bytes()));
    let (_, row) = answer.iter().find(|(tag, _)| *tag == b'D').expect("a row");
    std::str::from_utf8(&row[6..]).unwrap().parse().unwrap()
}

#[test]
fn function_calls_run_functions_by_oid_as_postgres_does() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    let error = |code: &str, message: &str| Err((code.to_string(), message.to_string()));
    let abs = oid(&mut conn, "SELECT 'abs(int4)'::regprocedure::oid");
    let upper = oid(&mut conn, "SELECT 'upper(text)'::regprocedure::oid");
    let series = oid(&mut conn, "SELECT 'generate_series(int4, int4)'::regprocedure::oid");
    assert_eq!(call(&mut conn, &function_call(abs, &[], &[Some(b"-42")], 0)), (Ok(Some(b"42".to_vec())), b'I'));
    let binary = function_call(abs, &[1], &[Some(&(-7i32).to_be_bytes())], 1);
    assert_eq!(call(&mut conn, &binary), (Ok(Some(7i32.to_be_bytes().to_vec())), b'I'));
    let collated = call(&mut conn, &function_call(upper, &[0], &[Some(b"abc")], 0));
    assert_eq!(collated, (error("42P22", "could not determine which collation to use for upper() function"), b'I'));
    assert_eq!(call(&mut conn, &function_call(abs, &[], &[None], 0)), (Ok(None), b'I'));
    let missing = call(&mut conn, &function_call(0, &[], &[], 0));
    assert_eq!(missing, (error("42883", "function with OID 0 does not exist"), b'I'));
    let set = call(&mut conn, &function_call(series, &[], &[Some(b"1"), Some(b"2")], 0));
    assert_eq!(set, (error("0A000", "cannot call function \"generate_series\" via fastpath interface"), b'I'));
    let counted = call(&mut conn, &function_call(abs, &[], &[], 0));
    assert_eq!(counted, (error("08P01", "function call message contains 0 arguments but function requires 1"), b'I'));
    let formats = call(&mut conn, &function_call(upper, &[0, 0], &[Some(b"a")], 0));
    assert_eq!(formats, (error("08P01", "function call message contains 2 argument formats but 1 arguments"), b'I'));
    let format = call(&mut conn, &function_call(abs, &[2], &[Some(b"1")], 0));
    assert_eq!(format, (error("22023", "unsupported format code: 2"), b'I'));
    let rformat = call(&mut conn, &function_call(abs, &[], &[Some(b"1")], 3));
    assert_eq!(rformat, (error("22023", "unsupported format code: 3"), b'I'));
    let long = call(&mut conn, &function_call(abs, &[1], &[Some(&[0, 0, 0, 1, 2])], 0));
    assert_eq!(long, (error("22P03", "incorrect binary data format in function argument 1"), b'I'));
    let input = call(&mut conn, &function_call(abs, &[], &[Some(b"x")], 0));
    assert_eq!(input, (error("22P02", "invalid input syntax for type integer: \"x\""), b'I'));
    let divided = run(&mut conn, b"BEGIN; SELECT 1 / 0");
    assert_eq!(divided, (vec![("22012".to_string(), "division by zero".to_string())], b'E'));
    let failed = call(&mut conn, &function_call(abs, &[], &[Some(b"1")], 0));
    let aborted = "current transaction is aborted, commands ignored until end of transaction block";
    assert_eq!(failed, (error("25P02", aborted), b'E'));
    assert_eq!(run(&mut conn, b"ROLLBACK"), (vec![], b'I'));
    assert_eq!(run(&mut conn, b"BEGIN"), (vec![], b'T'));
    assert_eq!(call(&mut conn, &function_call(abs, &[], &[Some(b"-1")], 0)), (Ok(Some(b"1".to_vec())), b'T'));
    assert_eq!(run(&mut conn, b"COMMIT"), (vec![], b'I'));
    let setup = b"CREATE ROLE fastpath_user; CREATE FUNCTION fastpath_f() RETURNS INT LANGUAGE SQL AS 'SELECT 1'; \
        REVOKE EXECUTE ON FUNCTION fastpath_f() FROM PUBLIC; CREATE SCHEMA fastpath_s; \
        CREATE FUNCTION fastpath_s.g() RETURNS INT LANGUAGE SQL AS 'SELECT 2'; \
        CREATE FUNCTION fastpath_state() RETURNS TEXT LANGUAGE SQL \
        AS 'SELECT state FROM pg_stat_activity WHERE pid = pg_backend_pid()'";
    assert_eq!(run(&mut conn, setup), (vec![], b'I'));
    let f = oid(&mut conn, "SELECT 'fastpath_f'::regproc::oid");
    let g = oid(&mut conn, "SELECT 'fastpath_s.g'::regproc::oid");
    assert_eq!(call(&mut conn, &function_call(f, &[], &[], 0)), (Ok(Some(b"1".to_vec())), b'I'));
    let state = oid(&mut conn, "SELECT 'fastpath_state'::regproc::oid");
    let answer = call(&mut conn, &function_call(state, &[], &[], 0));
    assert_eq!(answer, (Ok(Some(b"fastpath function call".to_vec())), b'I'));
    assert_eq!(run(&mut conn, b"SET ROLE fastpath_user"), (vec![], b'I'));
    let denied = call(&mut conn, &function_call(f, &[], &[], 0));
    assert_eq!(denied, (error("42501", "permission denied for function fastpath_f"), b'I'));
    let schema = call(&mut conn, &function_call(g, &[], &[], 0));
    assert_eq!(schema, (error("42501", "permission denied for schema fastpath_s"), b'I'));
}

#[test]
fn function_calls_read_and_write_text_in_the_client_encoding() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    for &(encoding, text, _) in ENCODED {
        let parameters = [("user", "postgres"), ("database", "postgres"), ("client_encoding", encoding)];
        let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
        let concat = oid(&mut conn, "SELECT 'textcat(text, text)'::regprocedure::oid");
        let answer = call(&mut conn, &function_call(concat, &[], &[Some(text), Some(b"!")], 0));
        assert_eq!(answer, (Ok(Some([text, b"!"].concat())), b'I'), "{encoding}");
    }
}

#[test]
fn function_calls_check_the_function_before_reading_its_arguments() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    let error = |code: &str, message: &str| Err((code.to_string(), message.to_string()));
    let abs = oid(&mut conn, "SELECT 'abs(int4)'::regprocedure::oid");
    let malformed = |fid: u32| [&fid.to_be_bytes()[..], &[0, 0, 0, 1], &(-2i32).to_be_bytes(), &[0, 0]].concat();
    let missing = call(&mut conn, &message(b'F', &malformed(0)));
    assert_eq!(missing, (error("42883", "function with OID 0 does not exist"), b'I'));
    let size = call(&mut conn, &message(b'F', &malformed(abs)));
    assert_eq!(size, (error("08P01", "invalid argument size -2 in function call message"), b'I'));
    assert_eq!(call(&mut conn, &function_call(abs, &[], &[Some(b"-3")], 0)), (Ok(Some(b"3".to_vec())), b'I'));
}

#[test]
fn malformed_message_bodies_fail_and_the_session_goes_on() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    let error = |message: &str| vec![("08P01".to_string(), message.to_string())];
    let sync = message(b'S', b"");
    assert_eq!(answer(&mut conn, &message(b'Q', b"SELECT 1")), (error("invalid string in message"), b'I'));
    assert_eq!(answer(&mut conn, &message(b'Q', b"SELECT 1\0x")), (error("invalid message format"), b'I'));
    let parse = [message(b'P', b"\0SELECT 1\0\0"), message(b'B', b"\0\0\0\0\0\0\0\0"), sync.clone()].concat();
    assert_eq!(answer(&mut conn, &parse), (error("insufficient data left in message"), b'I'));
    let typed = message(b'P', b"\0SELECT $1::INT4\0\0\0");
    let bind = message(b'B', &[&b"\0\0\0\0\0\x01"[..], &(-2i32).to_be_bytes(), b"\0\0"].concat());
    let negative = [typed, bind, message(b'E', b"\0\0\0\0\0"), sync.clone()].concat();
    assert_eq!(answer(&mut conn, &negative), (error("insufficient data left in message"), b'I'));
    let execute = [message(b'E', b"\0\0\0"), sync.clone()].concat();
    assert_eq!(answer(&mut conn, &execute), (error("insufficient data left in message"), b'I'));
    let skipped = [message(b'E', b"\0\0\0"), message(b'S', b"x")].concat();
    let both = [error("insufficient data left in message"), error("invalid message format")].concat();
    assert_eq!(answer(&mut conn, &skipped), (both, b'I'));
    let copy = [message(b'd', b"1"), message(b'f', b"oops"), message(b'c', b"x"), query(b"SELECT 1")].concat();
    assert_eq!(answer(&mut conn, &copy), (Vec::new(), b'I'));
    assert_eq!(run(&mut conn, b"BEGIN"), (Vec::new(), b'T'));
    assert_eq!(answer(&mut conn, &message(b'Q', b"SELECT 1")), (error("invalid string in message"), b'E'));
    assert_eq!(run(&mut conn, b"ROLLBACK"), (Vec::new(), b'I'));
    let flush = [message(b'H', b"x"), sync].concat();
    assert_eq!(answer(&mut conn, &flush), (error("invalid message format"), b'I'));
    assert_eq!(answer(&mut conn, &message(b'F', b"\0\0")), (error("insufficient data left in message"), b'I'));
    let abs = oid(&mut conn, "SELECT 'abs(int4)'::regprocedure::oid").to_be_bytes();
    let input = message(b'F', &[&abs[..], b"\0\0\0\x01\0\0\0\x01x\0"].concat());
    let invalid = vec![("22P02".to_string(), "invalid input syntax for type integer: \"x\"".to_string())];
    assert_eq!(answer(&mut conn, &input), (invalid, b'I'));
    let trailing = message(b'F', &[&abs[..], b"\0\0\0\x01\0\0\0\x02-3\0\0z"].concat());
    assert_eq!(answer(&mut conn, &trailing), (error("invalid message format"), b'I'));
}

#[test]
fn binds_find_the_statement_and_read_each_parameter_before_reading_on() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let parameters = [("user", "postgres"), ("database", "postgres")];
    let mut conn = RawConnection::connect(server.port, &parameters).unwrap();
    let sync = message(b'S', b"");
    let missing = [message(b'B', b"\0nosuch\0\0"), sync.clone()].concat();
    let unknown = vec![("26000".to_string(), "prepared statement \"nosuch\" does not exist".to_string())];
    assert_eq!(answer(&mut conn, &missing), (unknown, b'I'));
    let typed = message(b'P', b"\0SELECT $1::INT4\0\0\0");
    let input = [typed, message(b'B', b"\0\0\0\0\0\x01\0\0\0\x01x\0"), sync].concat();
    let invalid = vec![("22P02".to_string(), "invalid input syntax for type integer: \"x\"".to_string())];
    assert_eq!(answer(&mut conn, &input), (invalid, b'I'));
}
