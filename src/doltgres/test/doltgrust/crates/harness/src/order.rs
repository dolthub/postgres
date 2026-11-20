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

//! Decides which orders of a statement's rows are valid results: any order when Postgres returns them in no defined
//! order, the one order of a statement that defines it, and under ORDER BY, any order of each run of rows whose sort
//! keys are equal.
//!
//! The fork's replacement for Doltgrust's crates/harness/src/order.rs at 1cc06b328cd9, the same but for parsing with the
//! pg_query crate, Postgres' own grammar, instead of Doltgrust's `parser` and `nodes` crates.

use std::collections::HashMap;

use pg_query::NodeEnum;
use pg_query::protobuf::{LimitOption, SelectStmt, SetOperation, a_const};

/// Order is the order a statement returns its rows in.
#[derive(Clone, Debug, PartialEq)]
pub enum Order {
    /// The rows come in one defined order.
    Defined,
    /// The rows come in no defined order.
    Any,
    /// The rows are sorted by keys, and rows with equal keys come in no defined order. A cut start or end means the
    /// first or last run of equal keys may hold any of the tied rows, as an OFFSET or LIMIT that cuts the run picks.
    Sorted { keys: Vec<Key>, cut_start: bool, cut_end: bool },
}

/// Key is the output column a sort key names.
#[derive(Clone, Debug, PartialEq)]
pub enum Key {
    /// The output column at an index.
    Column(usize),
    /// The one output column of a name, or when no output column has the name, the column at an index whose expression
    /// is the key, as Postgres looks up a bare name in ORDER BY.
    Name(String, Option<usize>),
}

/// Statements reads a script's statements in order, remembering the order of the rows of each declared cursor and
/// prepared statement.
#[derive(Default)]
pub struct Statements {
    /// The order of each cursor's rows, by name.
    cursors: HashMap<String, Order>,
    /// The order of each prepared statement's rows, by name.
    prepared: HashMap<String, Order>,
}

impl Statements {
    /// order returns the order of the rows of a script's next unit, which is `Defined` unless the unit is one statement
    /// that returns rows in another order.
    pub fn order(&mut self, query: &str) -> Order {
        let Ok(parsed) = pg_query::parse(query, 0) else {
            return Order::Defined;
        };
        let stmts: Vec<Option<&NodeEnum>> =
            parsed.protobuf.stmts.iter().map(|s| s.stmt.as_ref().and_then(|n| n.node.as_ref())).collect();
        for stmt in &stmts {
            match stmt {
                Some(NodeEnum::DeclareCursorStmt(cursor)) => {
                    self.cursors.insert(cursor.portalname.clone(), statement_order(inner(&cursor.query)));
                }
                Some(NodeEnum::PrepareStmt(prepare)) => {
                    self.prepared.insert(prepare.name.clone(), statement_order(inner(&prepare.query)));
                }
                _ => {}
            }
        }
        let [stmt] = stmts.as_slice() else { return Order::Defined };
        match stmt {
            Some(NodeEnum::FetchStmt(fetch)) if !fetch.ismove => match self.cursors.get(&fetch.portalname) {
                Some(Order::Sorted { keys, .. }) => {
                    Order::Sorted { keys: keys.clone(), cut_start: true, cut_end: true }
                }
                Some(order) => order.clone(),
                None => Order::Defined,
            },
            Some(NodeEnum::ExecuteStmt(execute)) => self.prepared.get(&execute.name).cloned().unwrap_or(Order::Defined),
            stmt => statement_order(*stmt),
        }
    }
}

/// inner returns the node a boxed field holds.
fn inner(node: &Option<Box<pg_query::protobuf::Node>>) -> Option<&NodeEnum> {
    node.as_ref().and_then(|n| n.node.as_ref())
}

/// statement_order returns the order of a statement's rows: a SELECT's, `Any` for an INSERT, UPDATE, DELETE, or MERGE
/// with RETURNING, and `Defined` for any other statement.
fn statement_order(stmt: Option<&NodeEnum>) -> Order {
    let returning = match stmt {
        Some(NodeEnum::SelectStmt(select)) => return select_order(select),
        Some(NodeEnum::InsertStmt(s)) => s.returning_clause.is_some(),
        Some(NodeEnum::UpdateStmt(s)) => s.returning_clause.is_some(),
        Some(NodeEnum::DeleteStmt(s)) => s.returning_clause.is_some(),
        Some(NodeEnum::MergeStmt(s)) => s.returning_clause.is_some(),
        _ => false,
    };
    if returning { Order::Any } else { Order::Defined }
}

/// select_order returns the order of a SELECT's rows: `Any` without ORDER BY at its top level, `Sorted` when every
/// sort key names an output column, and otherwise `Defined`, since rows tied on a key that is not output cannot be
/// told apart from rows out of order.
fn select_order(select: &SelectStmt) -> Order {
    if select.sort_clause.is_empty() {
        return Order::Any;
    }
    let targets: &[pg_query::protobuf::Node] =
        if select.op == SetOperation::SetopNone as i32 { &select.target_list } else { &[] };
    let key = |sort: &pg_query::protobuf::Node| match sort.node.as_ref() {
        Some(NodeEnum::SortBy(sort)) => inner(&sort.node).and_then(|node| key(node, targets)),
        _ => None,
    };
    let Some(keys) = select.sort_clause.iter().map(key).collect::<Option<Vec<_>>>() else {
        return Order::Defined;
    };
    let limited = inner(&select.limit_count).is_some_and(|count| !matches!(count, NodeEnum::AConst(c) if c.isnull));
    Order::Sorted {
        keys,
        cut_start: select.limit_offset.is_some(),
        cut_end: limited && select.limit_option != LimitOption::WithTies as i32,
    }
}

/// key returns the output column a sort key names: by position, by output name, or by being the expression of a
/// target before any `*` target.
fn key(node: &NodeEnum, targets: &[pg_query::protobuf::Node]) -> Option<Key> {
    let column = targets.iter().take_while(|t| !star(t)).position(
        |t| matches!(t.node.as_ref(), Some(NodeEnum::ResTarget(t)) if inner(&t.val).is_some_and(|val| same(val, node))),
    );
    match node {
        NodeEnum::AConst(c) => match (c.isnull, &c.val) {
            (false, Some(a_const::Val::Ival(position))) => {
                usize::try_from(position.ival).ok()?.checked_sub(1).map(Key::Column)
            }
            _ => None,
        },
        NodeEnum::ColumnRef(c) => match c.fields.as_slice() {
            [field] => match field.node.as_ref() {
                Some(NodeEnum::String(name)) => Some(Key::Name(name.sval.clone(), column)),
                _ => column.map(Key::Column),
            },
            _ => column.map(Key::Column),
        },
        _ => column.map(Key::Column),
    }
}

/// star returns whether a target is `*` or a table's `*`.
fn star(target: &pg_query::protobuf::Node) -> bool {
    let Some(NodeEnum::ResTarget(target)) = target.node.as_ref() else { return false };
    matches!(inner(&target.val), Some(NodeEnum::ColumnRef(c))
        if matches!(c.fields.last().and_then(|f| f.node.as_ref()), Some(NodeEnum::AStar(_))))
}

/// same returns whether two expressions are the same apart from where in the statement they are written, as Doltgrust's
/// nodes compare, by comparing their trees with every location left out.
fn same(a: &NodeEnum, b: &NodeEnum) -> bool {
    without_locations(&format!("{a:?}")) == without_locations(&format!("{b:?}"))
}

/// without_locations returns a node's debug text with each `location: <number>` field removed.
fn without_locations(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(at) = rest.find("location: ") {
        out.push_str(&rest[..at]);
        rest = rest[at + "location: ".len()..].trim_start_matches(|c: char| c == '-' || c.is_ascii_digit());
    }
    out.push_str(rest);
    out
}

/// columns returns the output columns that sort keys name among a result's column names, or `None` when one names
/// no single column.
fn columns(keys: &[Key], names: &[&str]) -> Option<Vec<usize>> {
    let column = |key: &Key| match key {
        Key::Column(index) => Some(*index),
        Key::Name(name, expression) => match names.iter().filter(|n| *n == name).count() {
            0 => *expression,
            1 => names.iter().position(|n| n == name),
            _ => None,
        },
    };
    keys.iter().map(column).collect::<Option<Vec<_>>>().filter(|columns| columns.iter().all(|c| *c < names.len()))
}

/// matches returns whether received rows are a valid result in an order, given the expected rows Postgres returned
/// and the result's column names. `tied` says whether two expected rows have equal values in columns, and `check`
/// whether received rows match expected rows as a multiset, or given columns, whether every received row has the
/// first expected row's values in them.
pub fn matches<E, R>(
    order: &Order,
    names: &[&str],
    expected: &[E],
    received: &[R],
    tied: impl Fn(&E, &E, &[usize]) -> bool,
    mut check: impl FnMut(&[E], &[R], Option<&[usize]>) -> bool,
) -> bool {
    if expected.len() != received.len() {
        return false;
    }
    let sorted = match order {
        Order::Any => return check(expected, received, None),
        Order::Sorted { keys, cut_start, cut_end } => columns(keys, names).map(|keys| (keys, *cut_start, *cut_end)),
        Order::Defined => None,
    };
    let Some((keys, cut_start, cut_end)) = sorted else {
        return (0..expected.len()).all(|i| check(&expected[i..=i], &received[i..=i], None));
    };
    let mut start = 0;
    while start < expected.len() {
        let end = start + 1 + expected[start + 1..].iter().take_while(|e| tied(&expected[start], e, &keys)).count();
        let cut = (cut_start && start == 0) || (cut_end && end == expected.len());
        if !check(&expected[start..end], &received[start..end], cut.then_some(keys.as_slice())) {
            return false;
        }
        start = end;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    /// sorted is a sorted order by keys.
    fn sorted(keys: Vec<Key>, cut_start: bool, cut_end: bool) -> Order {
        Order::Sorted { keys, cut_start, cut_end }
    }

    /// name is a key by an output name.
    fn name(name: &str, expression: Option<usize>) -> Key {
        Key::Name(name.to_string(), expression)
    }

    /// valid returns whether received rows of text are a valid result for expected ones in an order.
    fn valid(order: &Order, names: &[&str], expected: &[&[&str]], received: &[&[&str]]) -> bool {
        let tied = |a: &&[&str], b: &&[&str], keys: &[usize]| keys.iter().all(|k| a[*k] == b[*k]);
        let check = |e: &[&[&str]], r: &[&[&str]], keys: Option<&[usize]>| match keys {
            Some(keys) => r.iter().all(|r| keys.iter().all(|k| r[*k] == e[0][*k])),
            None => {
                let (mut e, mut r) = (e.to_vec(), r.to_vec());
                e.sort_unstable();
                r.sort_unstable();
                e == r
            }
        };
        matches(order, names, expected, received, tied, check)
    }

    #[test]
    fn unordered_statements() {
        let mut statements = Statements::default();
        for (query, expected) in [
            ("SELECT * FROM t;", Order::Any),
            ("SELECT * FROM t ORDER BY random();", Order::Defined),
            ("SELECT * FROM (SELECT * FROM t ORDER BY a) s;", Order::Any),
            ("SELECT a FROM t UNION SELECT b FROM u ORDER BY 1;", sorted(vec![Key::Column(0)], false, false)),
            ("WITH w AS (SELECT 1) SELECT * FROM w;", Order::Any),
            ("SELECT array_agg(a ORDER BY b) FROM t;", Order::Any),
            ("EXPLAIN SELECT * FROM t;", Order::Defined),
            ("SELECT 1; SELECT 2;", Order::Defined),
            ("INSERT INTO t VALUES (1) RETURNING *;", Order::Any),
            ("DELETE FROM t;", Order::Defined),
            ("SELECT :x;", Order::Defined),
            ("SELECT * FROM t LIMIT 2;", Order::Any),
            ("DECLARE c CURSOR FOR SELECT * FROM t;", Order::Defined),
            ("FETCH ALL FROM c;", Order::Any),
            ("MOVE ALL IN c;", Order::Defined),
            ("DECLARE c CURSOR FOR SELECT * FROM t ORDER BY a;", Order::Defined),
            ("FETCH 2 FROM c;", sorted(vec![name("a", None)], true, true)),
            ("FETCH ALL FROM d;", Order::Defined),
            ("PREPARE p AS SELECT * FROM t;", Order::Defined),
            ("EXECUTE p;", Order::Any),
            ("EXECUTE q;", Order::Defined),
        ] {
            assert_eq!(statements.order(query), expected, "{query}");
        }
    }

    #[test]
    fn sort_keys_resolve_to_output_columns() {
        let mut statements = Statements::default();
        for (query, expected) in [
            ("SELECT a, b FROM t ORDER BY 2, 1;", sorted(vec![Key::Column(1), Key::Column(0)], false, false)),
            ("SELECT a AS x, b FROM t ORDER BY a;", sorted(vec![name("a", Some(0))], false, false)),
            ("SELECT a + 1, b FROM t ORDER BY a + 1 DESC;", sorted(vec![Key::Column(0)], false, false)),
            ("SELECT a, b FROM t ORDER BY t.b;", Order::Defined),
            ("SELECT t.a, b FROM t ORDER BY t.a;", sorted(vec![Key::Column(0)], false, false)),
            ("SELECT *, a + 1 FROM t ORDER BY a + 1;", Order::Defined),
            ("SELECT a FROM t ORDER BY b + 1;", Order::Defined),
            ("VALUES (1), (2) ORDER BY 1;", sorted(vec![Key::Column(0)], false, false)),
        ] {
            assert_eq!(statements.order(query), expected, "{query}");
        }
        assert_eq!(columns(&[name("x", None)], &["a", "x"]), Some(vec![1]));
        assert_eq!(columns(&[name("a", Some(0))], &["x", "b"]), Some(vec![0]));
        assert_eq!(columns(&[name("a", None)], &["x", "b"]), None);
        assert_eq!(columns(&[name("a", Some(0))], &["a", "a"]), None);
        assert_eq!(columns(&[Key::Column(2)], &["a", "b"]), None);
    }

    #[test]
    fn ties_come_in_any_order() {
        let order = sorted(vec![Key::Column(0)], false, false);
        let names = ["k", "v"];
        let expected: &[&[&str]] = &[&["1", "a"], &["1", "b"], &["2", "c"], &["3", "d"], &["3", "e"]];
        assert!(valid(&order, &names, expected, &[&["1", "b"], &["1", "a"], &["2", "c"], &["3", "e"], &["3", "d"]]));
        assert!(!valid(&order, &names, expected, &[&["1", "a"], &["2", "c"], &["1", "b"], &["3", "d"], &["3", "e"]]));
        assert!(!valid(&order, &names, expected, &[&["1", "a"], &["1", "b"], &["2", "c"], &["3", "d"], &["3", "f"]]));
        assert!(!valid(
            &Order::Defined,
            &names,
            expected,
            &[&["1", "b"], &["1", "a"], &["2", "c"], &["3", "d"], &["3", "e"]]
        ));
        assert!(valid(
            &Order::Any,
            &names,
            expected,
            &[&["3", "e"], &["1", "b"], &["2", "c"], &["3", "d"], &["1", "a"]]
        ));
    }

    #[test]
    fn unresolved_keys_keep_the_order() {
        let order = sorted(vec![name("z", None)], false, false);
        let expected: &[&[&str]] = &[&["1", "a"], &["1", "b"]];
        assert!(!valid(&order, &["k", "v"], expected, &[&["1", "b"], &["1", "a"]]));
        assert!(valid(&order, &["k", "v"], expected, expected));
    }

    #[test]
    fn cut_runs_may_hold_other_tied_rows() {
        let names = ["k", "v"];
        let expected: &[&[&str]] = &[&["1", "a"], &["2", "b"], &["2", "c"]];
        let received: &[&[&str]] = &[&["1", "a"], &["2", "c"], &["2", "z"]];
        assert!(valid(&sorted(vec![Key::Column(0)], false, true), &names, expected, received));
        assert!(!valid(&sorted(vec![Key::Column(0)], false, false), &names, expected, received));
        assert!(!valid(
            &sorted(vec![Key::Column(0)], false, true),
            &names,
            expected,
            &[&["1", "a"], &["2", "c"], &["3", "z"]]
        ));
        assert!(!valid(&sorted(vec![Key::Column(0)], false, true), &names, expected, &received[..2]));
        let offset: &[&[&str]] = &[&["1", "y"], &["2", "b"], &["2", "c"]];
        assert!(valid(
            &sorted(vec![Key::Column(0)], true, false),
            &names,
            &[&["1", "a"], &["2", "b"], &["2", "c"]],
            offset
        ));
        assert!(!valid(
            &sorted(vec![Key::Column(0)], true, false),
            &names,
            &[&["1", "a"], &["2", "b"], &["2", "c"]],
            &[&["1", "y"], &["2", "b"], &["2", "d"]]
        ));
    }

    #[test]
    fn limits_cut_the_last_run_unless_with_ties() {
        let mut statements = Statements::default();
        for (query, expected) in [
            ("SELECT a FROM t ORDER BY a LIMIT 2;", sorted(vec![name("a", Some(0))], false, true)),
            ("SELECT a FROM t ORDER BY a LIMIT ALL;", sorted(vec![name("a", Some(0))], false, false)),
            ("SELECT a FROM t ORDER BY a OFFSET 1;", sorted(vec![name("a", Some(0))], true, false)),
            ("SELECT a FROM t ORDER BY a FETCH FIRST 2 ROWS ONLY;", sorted(vec![name("a", Some(0))], false, true)),
            (
                "SELECT a FROM t ORDER BY a FETCH FIRST 2 ROWS WITH TIES;",
                sorted(vec![name("a", Some(0))], false, false),
            ),
            (
                "SELECT a FROM t ORDER BY a OFFSET 1 FETCH FIRST 2 ROWS WITH TIES;",
                sorted(vec![name("a", Some(0))], true, false),
            ),
        ] {
            assert_eq!(statements.order(query), expected, "{query}");
        }
    }

    #[test]
    fn fetches_from_sorted_cursors_cut_both_ends() {
        let mut statements = Statements::default();
        statements.order("DECLARE c CURSOR FOR SELECT k, v FROM t ORDER BY k;");
        let order = statements.order("FETCH 3 FROM c;");
        let names = ["k", "v"];
        let expected: &[&[&str]] = &[&["1", "a"], &["2", "b"], &["2", "c"], &["3", "d"]];
        assert!(valid(&order, &names, expected, &[&["1", "x"], &["2", "c"], &["2", "b"], &["3", "y"]]));
        assert!(!valid(&order, &names, expected, &[&["1", "x"], &["2", "c"], &["2", "z"], &["3", "y"]]));
    }
}
