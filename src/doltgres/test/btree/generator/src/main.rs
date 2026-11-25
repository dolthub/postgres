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

//! The btree differential's generator (the btree text's section 3): for each value of the corpus, which covers every
//! key type of Doltgrust's `has_encoding` list, a non-unique index's entry as Doltgrust makes it at the commit run.sh
//! names: the key column's key by Doltgrust's `encode_with_type`, then the row's key; and the value, an INCLUDE
//! column's int4 and, for a key type that isn't exact, the column's original, as Doltgrust's `write_tuple` places them
//! over `encoding::tuple::write`.

use encoding::key::Order;
use nodes::Datum;
use types::key::{KeyType, encode_with_type, is_exact_with_type};

/// ROW_KEY is every entry's row key, an int4 table key's.
const ROW_KEY: [u8; 5] = [1, 0, 0, 0, 7];

/// Value is a corpus value: a by-value type's word, or a by-reference type's stored bytes.
enum Value {
    Word(u64),
    Bytes(Vec<u8>),
}

/// corpus returns each key type of Doltgrust's `has_encoding` list with its length and the values it is checked
/// over: by-value types' words, fixed-length types' stored bytes, and variable-length types' bytes without their
/// header, short and long but under the bound.
fn corpus() -> Vec<(u32, i16, Vec<Value>)> {
    let words = |values: &[i64]| values.iter().map(|&v| Value::Word(v as u64)).collect::<Vec<_>>();
    let bytes = |values: Vec<Vec<u8>>| values.into_iter().map(Value::Bytes).collect::<Vec<_>>();
    let texts = || bytes(vec![Vec::new(), b"abc".to_vec(), b"abd".to_vec(), vec![b'z'; 255], vec![b'q'; 16_000]]);
    let name = |text: &[u8]| {
        let mut name = vec![0u8; 64];
        name[..text.len()].copy_from_slice(text);
        name
    };
    vec![
        (16, 1, words(&[0, 1])),
        (18, 1, words(&[b'a' as i64, -3])),
        (21, 2, words(&[-32768, -1, 0, 1, 32767])),
        (23, 4, words(&[i32::MIN as i64, -1, 0, 1, i32::MAX as i64])),
        (20, 8, words(&[i64::MIN, -1, 0, 1, i64::MAX])),
        (26, 4, words(&[0, 1, u32::MAX as i64])),
        (700, 4, words(&[(-1.5f32).to_bits() as i64, 0f32.to_bits() as i64, 2.25f32.to_bits() as i64])),
        (701, 8, words(&[(-1.5f64).to_bits() as i64, 0f64.to_bits() as i64, f64::MAX.to_bits() as i64])),
        (790, 8, words(&[-100, 0, 12_345])),
        (1082, 4, words(&[-1, 0, 9_000])),
        (1083, 8, words(&[0, 3_600_000_000, 86_399_999_999])),
        (1114, 8, words(&[-1_000_000, 0, 1_000_000_000_000])),
        (1184, 8, words(&[-1_000_000, 0, 1_000_000_000_000])),
        (3220, 8, words(&[0, 1, -1])),
        (5069, 8, words(&[0, 1, i64::MAX])),
        (2950, 16, bytes(vec![vec![0; 16], (0..16).collect(), vec![255; 16]])),
        (829, 6, bytes(vec![vec![0; 6], vec![8, 0, 43, 1, 2, 3]])),
        (774, 8, bytes(vec![vec![0; 8], vec![8, 0, 43, 1, 2, 3, 4, 5]])),
        (27, 6, bytes(vec![vec![0, 0, 0, 0, 1, 0], vec![0, 0, 1, 0, 7, 0]])),
        (19, 64, bytes(vec![name(b""), name(b"names"), name(&[b'n'; 63])])),
        (25, -1, texts()),
        (1043, -1, texts()),
        (1042, -1, texts()),
        (17, -1, bytes(vec![Vec::new(), vec![0, 1, 2, 255], vec![7; 4_000]])),
    ]
}

/// stored returns the values the reference's `stored.sql` printed into a file: each value's type, its length, and its
/// stored bytes, a variable-length value's without its header, which `stored.sql`'s short values never compress.
fn stored(path: &str) -> Vec<(u32, i16, Vec<Value>)> {
    let text = std::fs::read_to_string(path).expect("run.sh wrote the stored values");
    let mut types: Vec<(u32, i16, Vec<Value>)> = Vec::new();
    for line in text.lines() {
        let [typid, typlen, bytes] = line.split('\t').collect::<Vec<_>>()[..] else { panic!("a stored line: {line}") };
        let (typid, typlen): (u32, i16) = (typid.parse().unwrap(), typlen.parse().unwrap());
        let bytes: Vec<u8> =
            (0..bytes.len()).step_by(2).map(|i| u8::from_str_radix(&bytes[i..i + 2], 16).unwrap()).collect();
        let bytes = match (typlen, bytes[0]) {
            (-1, short) if short & 1 == 1 => bytes[1..usize::from(short >> 1)].to_vec(),
            (-1, long) if long & 3 == 0 => {
                bytes[4..(u32::from_le_bytes(bytes[..4].try_into().unwrap()) >> 2) as usize].to_vec()
            }
            (-1, _) => panic!("a stored value is compressed: {line}"),
            _ => bytes,
        };
        match types.last_mut() {
            Some((last, _, values)) if *last == typid => values.push(Value::Bytes(bytes)),
            _ => types.push((typid, typlen, vec![Value::Bytes(bytes)])),
        }
    }
    types
}

/// doltgrusts returns a stored value in Doltgrust's own datum form where it has one: a name's bytes up to its first
/// NUL, and a tsvector's and a tsquery's own big-endian forms (its `TsVector::read` and `TsQuery::read`) from Postgres'
/// stored ones; any other value's bytes are its stored ones.
fn doltgrusts(typid: u32, bytes: &[u8]) -> Vec<u8> {
    let u32_at = |at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());
    let u16_at = |at: usize| u16::from_le_bytes([bytes[at], bytes[at + 1]]);
    let mut out = Vec::new();
    match typid {
        19 => bytes[..bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())].to_vec(),
        3614 => {
            let size = u32_at(0) as usize;
            out.extend_from_slice(&(size as u32).to_be_bytes());
            let strings = 4 + 4 * size;
            for i in 0..size {
                let entry = u32_at(4 + 4 * i);
                let (positions, len, pos) = (entry & 1 == 1, (entry >> 1 & 0x7ff) as usize, (entry >> 12) as usize);
                let at = strings + (pos + len + 1 & !1);
                let count = if positions { usize::from(u16_at(at)) } else { 0 };
                out.extend_from_slice(&(len as u16).to_be_bytes());
                out.extend_from_slice(&(count as u16).to_be_bytes());
                out.extend_from_slice(&bytes[strings + pos..strings + pos + len]);
                (0..count).for_each(|j| out.extend_from_slice(&u16_at(at + 2 + 2 * j).to_be_bytes()));
            }
            out
        }
        3615 => {
            let size = u32_at(0) as usize;
            out.extend_from_slice(&(size as u32).to_be_bytes());
            let operands = 4 + 12 * size;
            for i in 0..size {
                let item = &bytes[4 + 12 * i..4 + 12 * (i + 1)];
                out.push(item[0]);
                match item[0] {
                    1 => {
                        let bits = u32::from_le_bytes(item[8..12].try_into().unwrap());
                        let (len, distance) = ((bits & 0xfff) as usize, (bits >> 12) as usize);
                        out.extend_from_slice(&[item[1], item[2]]);
                        out.extend_from_slice(&(len as u16).to_be_bytes());
                        out.extend_from_slice(&bytes[operands + distance..operands + distance + len]);
                    }
                    _ => {
                        out.push(item[1]);
                        out.extend_from_slice(&i16::from_le_bytes([item[2], item[3]]).to_be_bytes());
                    }
                }
            }
            out
        }
        _ => bytes.to_vec(),
    }
}

/// fields_of returns a tuple's fields as Doltgrust's `fields_of` makes them: a by-value type's first bytes of its
/// little-endian word, and a by-reference type's own bytes.
fn field_of(typlen: i16, value: &Value) -> Vec<u8> {
    match value {
        Value::Word(word) => word.to_le_bytes()[..typlen as usize].to_vec(),
        Value::Bytes(bytes) => bytes.clone(),
    }
}

/// hex returns bytes as lowercase hexadecimal.
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// main writes the corpus, its own values and those `stored.sql` printed into the file run.sh names, with Doltgrust's
/// entries: a header naming Doltgrust's commit and the generator's hash, then one line for each type, order, and value
/// (NULL among them), with an INCLUDE int4 of 9, each value given to Doltgrust in its own datum form.
fn main() {
    let args: Vec<String> = std::env::args().collect();
    println!(
        "# Written by test/btree/generator (sources' SHA-256 {}) from Doltgrust at {}; run.sh writes it again.",
        args[2], args[1]
    );
    println!(
        "# Outside it: unique indexes, whose value holds the row's key (the btree text's section 3), and cut keys"
    );
    println!(
        "# (design item 4's form D), which Doltgrust doesn't cut. The fork's own, which its case names: its value's"
    );
    println!("# field past Doltgrust's, NULL for an uncut exact key column that can be cut (section 3).");
    println!("# typid\ttyplen\torder\tvalue (w and a by-value word, or b and stored bytes)\tkey\tvalue's tuple");
    let orders = [
        ("asc", Order::ASC),
        ("desc", Order::DESC),
        ("asc_nulls_first", Order { descending: false, nulls_first: true }),
    ];
    let include = 9i32.to_le_bytes();
    for (typid, typlen, values) in corpus().into_iter().chain(stored(&args[3])) {
        let key_type = KeyType::of(typid);
        let lossy = !is_exact_with_type(&key_type);
        for (name, order) in orders {
            for value in values.iter().map(Some).chain([None]) {
                let own = match value {
                    Some(Value::Bytes(bytes)) => doltgrusts(typid, bytes),
                    _ => Vec::new(),
                };
                let datum = value.map(|value| match value {
                    Value::Word(word) => Datum::Word(*word),
                    Value::Bytes(_) => Datum::Bytes(&own),
                });
                let mut key = Vec::new();
                encode_with_type(&mut key, &key_type, datum, order).expect("the corpus's values encode");
                key.extend_from_slice(&ROW_KEY);
                let original = value.map(|value| field_of(typlen, value));
                let mut fields: Vec<Option<&[u8]>> = vec![Some(&include)];
                if lossy {
                    fields.push(original.as_deref());
                }
                let tuple = encoding::tuple::write(&fields, None);
                let shown = match value {
                    None => "NULL".to_owned(),
                    Some(Value::Word(word)) => format!("w{word:016x}"),
                    Some(Value::Bytes(bytes)) => format!("b{}", hex(bytes)),
                };
                println!("{typid}\t{typlen}\t{name}\t{shown}\t{}\t{}", hex(&key), hex(&tuple));
            }
        }
    }
}
