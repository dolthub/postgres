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

//! Splitting a stream into frames.

use pgproto::FrameReader;

#[test]
fn startup_packets_of_a_bad_length_fail_before_they_are_read() {
    for length in [7_i32, 10_005, i32::MAX] {
        let mut reader = FrameReader::new();
        reader.extend(&length.to_be_bytes());
        let error = reader.next_untyped_frame().unwrap_err();
        assert_eq!(error.message, "invalid length of startup packet", "{length}");
    }
    let mut reader = FrameReader::new();
    reader.extend(&10_004_i32.to_be_bytes());
    assert_eq!(reader.next_untyped_frame().unwrap(), None);
}
