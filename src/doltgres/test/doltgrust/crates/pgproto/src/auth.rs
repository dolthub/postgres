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

//! Password authentication's math, which the server's verifier and every client share: Postgres' MD5 hash and
//! SCRAM-SHA-256's keys. It also holds the client side of a SCRAM exchange, libpq's `fe-auth-scram.c`.

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use hmac::{Hmac, Mac};
use md5::{Digest, Md5};
use rand::RngCore;
use sha2::Sha256;

/// SCRAM_SHA_256 is the name of SCRAM-SHA-256 without channel binding.
pub const SCRAM_SHA_256: &str = "SCRAM-SHA-256";

/// SCRAM_SHA_256_PLUS is the name of SCRAM-SHA-256 with channel binding.
pub const SCRAM_SHA_256_PLUS: &str = "SCRAM-SHA-256-PLUS";

/// SCRAM_RAW_NONCE_LEN is how many random bytes a client nonce encodes, Postgres' `SCRAM_RAW_NONCE_LEN`.
const SCRAM_RAW_NONCE_LEN: usize = 18;

/// CBIND_HEADER is the GS2 header of a client that binds the exchange to the server's certificate.
const CBIND_HEADER: &str = "p=tls-server-end-point,,";

/// md5_encrypt returns `md5` followed by the hex MD5 digest of a password and a salt, Postgres' `pg_md5_encrypt`.
pub fn md5_encrypt(password: &[u8], salt: &[u8]) -> String {
    let mut digest = Md5::new();
    digest.update(password);
    digest.update(salt);
    format!("md5{}", digest.finalize().iter().map(|b| format!("{b:02x}")).collect::<String>())
}

/// md5_password returns the response to an MD5 password request, libpq's `pg_password_sendauth`: the password hashed
/// with the user's name as Postgres stores it, hashed again with the request's salt.
pub fn md5_password(user: &str, password: &str, salt: &[u8; 4]) -> String {
    let stored = md5_encrypt(password.as_bytes(), user.as_bytes());
    md5_encrypt(&stored.as_bytes()[3..], salt)
}

/// hmac_sha256 returns the HMAC-SHA-256 of a message under a key.
pub fn hmac_sha256(key: &[u8], message: &[u8]) -> [u8; 32] {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("HMAC takes keys of any length");
    mac.update(message);
    mac.finalize().into_bytes().into()
}

/// salted_password returns SCRAM's SaltedPassword of a password SASLprep already prepared, Postgres'
/// `scram_SaltedPassword`.
pub fn salted_password(password: &str, salt: &[u8], iterations: u32) -> [u8; 32] {
    let mut salted = [0u8; 32];
    pbkdf2::pbkdf2_hmac::<Sha256>(password.as_bytes(), salt, iterations, &mut salted);
    salted
}

/// client_key returns SCRAM's ClientKey of a SaltedPassword, Postgres' `scram_ClientKey`.
pub fn client_key(salted: &[u8; 32]) -> [u8; 32] {
    hmac_sha256(salted, b"Client Key")
}

/// server_key returns SCRAM's ServerKey of a SaltedPassword, Postgres' `scram_ServerKey`.
pub fn server_key(salted: &[u8; 32]) -> [u8; 32] {
    hmac_sha256(salted, b"Server Key")
}

/// stored_key returns SCRAM's StoredKey of a ClientKey, Postgres' `scram_H`.
pub fn stored_key(client_key: &[u8; 32]) -> [u8; 32] {
    Sha256::digest(client_key).into()
}

/// Binding is how a SCRAM client binds its exchange to the connection's TLS channel: not at all, not at all though it
/// could, which a server that offers binding takes as a downgrade, or to the server certificate's hash.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Binding {
    None,
    Supported,
    Certificate(Vec<u8>),
}

/// State is how far a SCRAM client's exchange has come.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Init,
    NonceSent,
    ProofSent,
    Finished,
}

/// ScramClient is the client side of a SCRAM-SHA-256 exchange, libpq's `fe_scram_state`, with its error messages.
pub struct ScramClient {
    password: String,
    user: String,
    binding: Binding,
    client_nonce: String,
    client_first_message_bare: String,
    server_first_message: String,
    client_final_without_proof: String,
    salted_password: [u8; 32],
    state: State,
}

impl ScramClient {
    /// new returns a client of a password SASLprep already prepared that binds as told, with a fresh random nonce, as
    /// libpq's `scram_init` and `build_client_first_message` make it.
    pub fn new(password: &str, binding: Binding) -> ScramClient {
        let mut nonce = [0u8; SCRAM_RAW_NONCE_LEN];
        rand::thread_rng().fill_bytes(&mut nonce);
        ScramClient {
            password: password.to_string(),
            user: String::new(),
            binding,
            client_nonce: STANDARD.encode(nonce),
            client_first_message_bare: String::new(),
            server_first_message: String::new(),
            client_final_without_proof: String::new(),
            salted_password: [0; 32],
            state: State::Init,
        }
    }

    /// with_user returns a client that names the user in its client-first-message, as some SCRAM libraries do though
    /// Postgres ignores it, and binds nothing.
    pub fn with_user(password: &str, user: &str) -> ScramClient {
        ScramClient { user: user.replace('=', "=3D").replace(',', "=2C"), ..ScramClient::new(password, Binding::None) }
    }

    /// mechanism returns the name of the mechanism the client uses.
    pub fn mechanism(&self) -> &'static str {
        match self.binding {
            Binding::Certificate(_) => SCRAM_SHA_256_PLUS,
            _ => SCRAM_SHA_256,
        }
    }

    /// channel_bound reports whether the exchange binds the TLS channel, libpq's `scram_channel_bound`.
    pub fn channel_bound(&self) -> bool {
        matches!(self.binding, Binding::Certificate(_)) && self.state == State::Finished
    }

    /// client_first_message returns the data of the SASLInitialResponse, libpq's `build_client_first_message`.
    pub fn client_first_message(&mut self) -> Vec<u8> {
        let header = match self.binding {
            Binding::None => "n,,",
            Binding::Supported => "y,,",
            Binding::Certificate(_) => "p=tls-server-end-point,,",
        };
        self.client_first_message_bare = format!("n={},r={}", self.user, self.client_nonce);
        self.state = State::NonceSent;
        format!("{header}{}", self.client_first_message_bare).into_bytes()
    }

    /// client_final_message reads the server-first-message and returns the data of the SASLResponse, libpq's
    /// `read_server_first_message` and `build_client_final_message`.
    pub fn client_final_message(&mut self, server_first_message: &[u8]) -> Result<Vec<u8>, String> {
        let input = message_text(server_first_message)?;
        if self.state != State::NonceSent {
            return Err("invalid SCRAM exchange state".to_string());
        }
        let mut rest = input;
        let nonce = read_attr_value(&mut rest, 'r')?;
        if nonce.len() < self.client_nonce.len() || !nonce.starts_with(&self.client_nonce) {
            return Err("invalid SCRAM response (nonce mismatch)".to_string());
        }
        let salt = STANDARD
            .decode(read_attr_value(&mut rest, 's')?)
            .map_err(|_| "malformed SCRAM message (invalid salt)".to_string())?;
        let iterations = match read_attr_value(&mut rest, 'i')?.parse::<i64>() {
            Ok(iterations @ 1..=0xffff_ffff) => iterations as u32,
            _ => return Err("malformed SCRAM message (invalid iteration count)".to_string()),
        };
        self.server_first_message = input.to_string();
        self.salted_password = salted_password(&self.password, &salt, iterations);
        let binding = match &self.binding {
            Binding::None => "biws".to_string(),
            Binding::Supported => "eSws".to_string(),
            Binding::Certificate(hash) => STANDARD.encode([CBIND_HEADER.as_bytes(), hash].concat()),
        };
        self.client_final_without_proof = format!("c={binding},r={nonce}");
        let client_key = client_key(&self.salted_password);
        let signature = hmac_sha256(&stored_key(&client_key), self.auth_message().as_bytes());
        let proof: Vec<u8> = client_key.iter().zip(signature).map(|(a, b)| a ^ b).collect();
        self.state = State::ProofSent;
        Ok(format!("{},p={}", self.client_final_without_proof, STANDARD.encode(proof)).into_bytes())
    }

    /// verify_server_final_message checks the server-final-message's signature, libpq's `read_server_final_message`
    /// and `verify_server_signature`.
    pub fn verify_server_final_message(&mut self, server_final_message: &[u8]) -> Result<(), String> {
        let mut rest = message_text(server_final_message)?;
        if self.state != State::ProofSent {
            return Err("invalid SCRAM exchange state".to_string());
        }
        if rest.starts_with('e') {
            let error = read_attr_value(&mut rest, 'e')?;
            return Err(format!("error received from server in SCRAM exchange: {error}"));
        }
        let signature = STANDARD.decode(read_attr_value(&mut rest, 'v')?).unwrap_or_default();
        if signature.len() != 32 {
            return Err("malformed SCRAM message (invalid server signature)".to_string());
        }
        let expected = hmac_sha256(&server_key(&self.salted_password), self.auth_message().as_bytes());
        if expected.iter().zip(&signature).fold(0, |differ, (a, b)| differ | (a ^ b)) != 0 {
            return Err("incorrect server signature".to_string());
        }
        self.state = State::Finished;
        Ok(())
    }

    /// auth_message returns SCRAM's AuthMessage of the exchange so far.
    fn auth_message(&self) -> String {
        format!("{},{},{}", self.client_first_message_bare, self.server_first_message, self.client_final_without_proof)
    }
}

/// message_text returns a server's SCRAM message as text, failing for one that is empty or holds a zero byte, as libpq's
/// `scram_exchange` checks it.
fn message_text(message: &[u8]) -> Result<&str, String> {
    if message.is_empty() {
        return Err("malformed SCRAM message (empty message)".to_string());
    }
    if message.contains(&0) {
        return Err("malformed SCRAM message (length mismatch)".to_string());
    }
    std::str::from_utf8(message).map_err(|_| "malformed SCRAM message (length mismatch)".to_string())
}

/// read_attr_value reads an attribute's value from a SCRAM message and moves past it and its comma, libpq's
/// `read_attr_value`.
fn read_attr_value<'a>(input: &mut &'a str, attr: char) -> Result<&'a str, String> {
    let Some(rest) = input.strip_prefix(attr) else {
        return Err(format!("malformed SCRAM message (attribute \"{attr}\" expected)"));
    };
    let Some(rest) = rest.strip_prefix('=') else {
        return Err(format!("malformed SCRAM message (expected character \"=\" for attribute \"{attr}\")"));
    };
    let (value, after) = rest.split_once(',').unwrap_or((rest, ""));
    *input = after;
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn md5_password_hashes_the_stored_hash_with_the_salt() {
        assert_eq!(md5_encrypt(b"secret", b"alice"), "md54a0a68b43b6cd5cf266fa02f196e2371");
        let salt = [1, 2, 3, 4];
        assert_eq!(md5_password("alice", "secret", &salt), md5_encrypt(b"4a0a68b43b6cd5cf266fa02f196e2371", &salt));
    }

    #[test]
    fn a_scram_client_refuses_a_server_nonce_that_does_not_start_with_its_own() {
        let mut client = ScramClient::new("pw", Binding::None);
        let first = client.client_first_message();
        assert!(first.starts_with(b"n,,n=,r="));
        let refused = client.client_final_message(b"r=other,s=c2FsdA==,i=4096").unwrap_err();
        assert_eq!(refused, "invalid SCRAM response (nonce mismatch)");
    }

    #[test]
    fn a_scram_client_proves_the_password_and_checks_the_servers_signature() {
        let mut client = ScramClient::new("pw", Binding::None);
        client.client_first_message();
        let nonce = format!("{}server", client.client_nonce);
        let server_first = format!("r={nonce},s={},i=4096", STANDARD.encode(b"salt"));
        let final_message = String::from_utf8(client.client_final_message(server_first.as_bytes()).unwrap()).unwrap();
        assert!(final_message.starts_with(&format!("c=biws,r={nonce},p=")));
        let salted = salted_password("pw", b"salt", 4096);
        let auth_message = format!("n=,r={},{server_first},c=biws,r={nonce}", client.client_nonce);
        let signature = STANDARD.encode(hmac_sha256(&server_key(&salted), auth_message.as_bytes()));
        let wrong = STANDARD.encode([0u8; 32]);
        let refused = client.verify_server_final_message(format!("v={wrong}").as_bytes());
        assert_eq!(refused, Err("incorrect server signature".to_string()));
        assert_eq!(
            client.verify_server_final_message(b"e=invalid-proof"),
            Err("error received from server in SCRAM exchange: invalid-proof".to_string())
        );
        assert_eq!(client.verify_server_final_message(format!("v={signature}").as_bytes()), Ok(()));
    }
}
