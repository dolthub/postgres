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

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_update() {
    run_scripts(&[
        ScriptTest {
            name: "simple update",
            set_up_script: &[
                "CREATE TABLE t1 (a INT PRIMARY KEY, b INT)",
                "INSERT INTO t1 VALUES (1, 2), (2, 3), (3, 4)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t1 SET b = 5 WHERE a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 where a =  2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update to default",
            set_up_script: &[
                "create table t (i int default 10, j varchar(128) default (concat('abc', 'def')));",
                "insert into t values (100, 'a'), (200, 'b');",
                "create table t2 (i int);",
                "insert into t2 values (1), (2), (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "update t set i = default where i = 100;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t order by i",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", VARCHAR)],
                        rows: &[
                            &[T("10"), T("a")],
                            &[T("200"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "update t set j = default where i = 200;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t order by i",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", VARCHAR)],
                        rows: &[
                            &[T("10"), T("a")],
                            &[T("200"), T("abcdef")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "update t set i = default, j = default;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t order by i",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", VARCHAR)],
                        rows: &[
                            &[T("10"), T("abcdef")],
                            &[T("10"), T("abcdef")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "update t2 set i = default",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t2",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[Null],
                            &[Null],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE ... RETURNING",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, c1 TEXT);",
                "INSERT INTO t VALUES (1, 'one');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET pk = pk+1, c1 = '42' RETURNING c1, pk, pk * 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", TEXT), Column("pk", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("42"), T("2"), T("4")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET c1 = '43' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("2"), T("43")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE ... RETURNING with join",
            set_up_script: &[
                "CREATE TABLE employees (id SERIAL PRIMARY KEY, name TEXT, department_id INT, salary INT);",
                "CREATE TABLE departments (id SERIAL PRIMARY KEY, name TEXT, bonus INT);",
                "INSERT INTO employees (name, department_id, salary) VALUES ('Alice', 1, 50000), ('Bob', 2, 60000);",
                "INSERT INTO departments (name, bonus) VALUES ('Engineering', 5000), ('Marketing', 3000);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE employees e SET salary = salary + d.bonus FROM departments d WHERE e.department_id = d.id RETURNING e.id, e.name, e.salary;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("salary", INT4)],
                        rows: &[
                            &[T("1"), T("Alice"), T("55000")],
                            &[T("2"), T("Bob"), T("63000")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on subquery",
            set_up_script: &[
                "CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, department_id INT, salary INT);",
                "CREATE TABLE departments (id INT PRIMARY KEY, name TEXT, bonus INT);",
                "INSERT INTO employees VALUES (1, 'Alice', 10, 50000), (2, 'Bob', 20, 60000);",
                "INSERT INTO departments VALUES (10, 'Engineering', 5000), (20, 'HR', 3000);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"
			UPDATE employees SET salary = salary + dept_bonus.bonus
			FROM ( SELECT id, bonus FROM departments ) AS dept_bonus
			WHERE employees.department_id = dept_bonus.id AND employees.name = 'Alice';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT salary FROM employees WHERE name = 'Alice';",
                    expected: Expected::Rows {
                        columns: &[Column("salary", INT4)],
                        rows: &[
                            &[T("55000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on one table",
            set_up_script: &[
                "CREATE TABLE products (id SERIAL PRIMARY KEY, name TEXT, price INT, category_id INT);",
                "CREATE TABLE categories (id SERIAL PRIMARY KEY, name TEXT, discount INT);",
                "INSERT INTO products (name, price, category_id) VALUES ('Laptop', 1000, 1), ('Phone', 800, 2);",
                "INSERT INTO categories (name, discount) VALUES ('Electronics', 100), ('Mobiles', 50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE products p SET price = price - c.discount FROM categories c WHERE p.category_id = c.id;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name, price FROM products ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("price", INT4)],
                        rows: &[
                            &[T("1"), T("Laptop"), T("900")],
                            &[T("2"), T("Phone"), T("750")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on two tables",
            set_up_script: &[
                "CREATE TABLE books (id SERIAL PRIMARY KEY, title TEXT, price INT, author_id INT, publisher_id INT);",
                "CREATE TABLE authors (id SERIAL PRIMARY KEY, royalty INT);",
                "CREATE TABLE publishers (id SERIAL PRIMARY KEY, markup INT);",
                "INSERT INTO books (title, price, author_id, publisher_id) VALUES ('Book A', 100, 1, 1), ('Book B', 120, 2, 2);",
                "INSERT INTO authors (royalty) VALUES (10), (20);",
                "INSERT INTO publishers (markup) VALUES (15), (25);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE books b SET price = price + a.royalty - p.markup FROM authors a, publishers p WHERE b.author_id = a.id AND b.publisher_id = p.id;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, title, price FROM books ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("title", TEXT), Column("price", INT4)],
                        rows: &[
                            &[T("1"), T("Book A"), T("95")],
                            &[T("2"), T("Book B"), T("115")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on three tables",
            set_up_script: &[
                "CREATE TABLE orders (id SERIAL PRIMARY KEY, customer_id INT, product_id INT, total INT);",
                "CREATE TABLE customers (id SERIAL PRIMARY KEY, loyalty_discount INT);",
                "CREATE TABLE products (id SERIAL PRIMARY KEY, base_price INT, tax_id INT);",
                "CREATE TABLE taxes (id SERIAL PRIMARY KEY, rate INT);",
                "INSERT INTO orders (customer_id, product_id, total) VALUES (1, 1, 0), (2, 2, 0);",
                "INSERT INTO customers (loyalty_discount) VALUES (5), (10);",
                "INSERT INTO products (base_price, tax_id) VALUES (100, 1), (200, 2);",
                "INSERT INTO taxes (rate) VALUES (10), (20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"
				UPDATE orders o
				SET total = p.base_price + (p.base_price * t.rate / 100) - c.loyalty_discount
				FROM customers c, products p, taxes t
				WHERE o.customer_id = c.id AND o.product_id = p.id AND p.tax_id = t.id;
			"#,
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, total FROM orders ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("total", INT4)],
                        rows: &[
                            &[T("1"), T("105")],
                            &[T("2"), T("230")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on four tables",
            set_up_script: &[
                "CREATE TABLE rentals (id SERIAL PRIMARY KEY, vehicle_id INT, user_id INT, total_cost INT);",
                "CREATE TABLE vehicles (id SERIAL PRIMARY KEY, base_rate INT, fuel_type_id INT);",
                "CREATE TABLE users (id SERIAL PRIMARY KEY, membership_level_id INT);",
                "CREATE TABLE fuel_types (id SERIAL PRIMARY KEY, surcharge INT);",
                "CREATE TABLE membership_levels (id SERIAL PRIMARY KEY, discount INT);",
                "INSERT INTO rentals (vehicle_id, user_id, total_cost) VALUES (1, 1, 0), (2, 2, 0);",
                "INSERT INTO vehicles (base_rate, fuel_type_id) VALUES (300, 1), (400, 2);",
                "INSERT INTO fuel_types (surcharge) VALUES (20), (40);",
                "INSERT INTO users (membership_level_id) VALUES (1), (2);",
                "INSERT INTO membership_levels (discount) VALUES (50), (80);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"
				UPDATE rentals r
				SET total_cost = v.base_rate + f.surcharge - m.discount
				FROM vehicles v, fuel_types f, users u, membership_levels m
				WHERE r.vehicle_id = v.id AND v.fuel_type_id = f.id AND r.user_id = u.id AND u.membership_level_id = m.id;
			"#,
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, total_cost FROM rentals ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("total_cost", INT4)],
                        rows: &[
                            &[T("1"), T("270")],
                            &[T("2"), T("360")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with join on table with trigger",
            set_up_script: &[
                r#"CREATE TABLE departments (id SERIAL PRIMARY KEY, name TEXT, bonus INT
);"#,
                "CREATE TABLE employees (id SERIAL PRIMARY KEY, name TEXT, department_id INT REFERENCES departments(id), salary INT);",
                "INSERT INTO departments (name, bonus) VALUES ('Engineering', 1000), ('HR', 500);",
                "INSERT INTO employees (name, department_id, salary) VALUES ('Alice', 1, 50000), ('Bob', 2, 45000);",
                "CREATE TABLE salary_log (employee_id INT, old_salary INT, new_salary INT);",
                r#"CREATE OR REPLACE FUNCTION log_salary_change()
					RETURNS TRIGGER AS $$
					BEGIN
						IF NEW.salary != OLD.salary THEN
							INSERT INTO salary_log VALUES (OLD.id, OLD.salary, NEW.salary);
						END IF;
						RETURN NEW;
					END;
					$$ LANGUAGE plpgsql;"#,
                r#"CREATE TRIGGER trg_log_salary_change
					AFTER UPDATE ON employees
					FOR EACH ROW
					EXECUTE FUNCTION log_salary_change();"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE employees e SET salary = salary + d.bonus FROM departments d WHERE e.department_id = d.id;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM salary_log;",
                    expected: Expected::Rows {
                        columns: &[Column("employee_id", INT4), Column("old_salary", INT4), Column("new_salary", INT4)],
                        rows: &[
                            &[T("1"), T("50000"), T("51000")],
                            &[T("2"), T("45000"), T("45500")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_update_affected_rows() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE and ON CONFLICT command counts",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 10), (2, 20)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 10",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 10 WHERE id = 999",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = $1 WHERE id = $2",
                    bind_vars: &[BindVar::Int(10), BindVar::Int(1)],
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1 RETURNING id, a",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1 RETURNING id, a",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 10) ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 11) ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 12) ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a WHERE false",
                    expected: Expected::Tag("INSERT 0 0"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "mixed insert and conflict update counts",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 10), (2, 22), (3, 33), (4, 40) ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a WHERE t.id <> 3",
                    expected: Expected::Tag("INSERT 0 3"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM t ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("22")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL FOUND after unchanged UPDATE",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 10)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION update_found(target_id INT) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN UPDATE t SET a = a WHERE id = target_id; RETURN FOUND; END; $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT update_found(1)",
                    expected: Expected::Rows {
                        columns: &[Column("update_found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT update_found(999)",
                    expected: Expected::Rows {
                        columns: &[Column("update_found", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "NULL and transaction counts without a primary key",
            set_up_script: &[
                "CREATE TABLE t (a INT)",
                "INSERT INTO t VALUES (NULL), (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = NULL WHERE a IS NULL",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE UPDATE trigger skips a row",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 10)",
                r#"CREATE FUNCTION skip_update() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RETURN NULL; END; $$;"#,
                "CREATE TRIGGER skip_update BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION skip_update()",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1 RETURNING id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1 RETURNING id",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE UPDATE trigger skips one of several rows",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)",
                r#"CREATE FUNCTION skip_one() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN IF OLD.id = 1 THEN RETURN NULL; END IF; RETURN NEW; END; $$;"#,
                "CREATE TRIGGER skip_one BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION skip_one()",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a = CASE WHEN id = 3 THEN 31 ELSE a END",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM t ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE FROM counts and RETURNING",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT)",
                "CREATE TABLE u (id INT)",
                "INSERT INTO t VALUES (1, 10), (2, 20)",
                "INSERT INTO u VALUES (1), (1), (2)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a = t.a FROM u WHERE t.id = u.id",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 10 FROM u WHERE t.id = u.id",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = t.a FROM u WHERE t.id = u.id RETURNING t.id, t.a",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("10")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = t.a FROM u WHERE t.id = u.id RETURNING t.id, t.a",
                    expected: Expected::Tag("UPDATE 2"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_update_assignment_semantics() {
    run_scripts(&[
        ScriptTest {
            name: "customer CASE",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = CASE WHEN a = 1 THEN 100 ELSE -1 END",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "reversed CASE",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET b = CASE WHEN a = 1 THEN 100 ELSE -1 END, a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "swap",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = b, b = a",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "reversed swap",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET b = a, a = b",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "arithmetic chain",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = a + 1, b = a + 10",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "NULL propagation",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = NULL, b = CASE WHEN a IS NULL THEN 100 ELSE -1 END",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[Null, T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple rows",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
                "INSERT INTO t_seq VALUES (3, 9)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = a + 1, b = a",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("4"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "scalar correlated subquery",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = (SELECT a + 10)",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHERE subquery",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
                "CREATE TABLE src (x int PRIMARY KEY)",
                "INSERT INTO src VALUES (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = a WHERE a IN (SELECT x FROM src)",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "assignment conversion",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 1.6, b = a",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated stored column",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int, c int GENERATED ALWAYS AS (a+b) STORED)",
                "INSERT INTO t_seq (a,b) VALUES (1,0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = a",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a,b,c FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("1"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "join same target",
            set_up_script: &[
                "CREATE TABLE t_seq (id int PRIMARY KEY, a int, b int)",
                "INSERT INTO t_seq VALUES (1,1,0)",
                "CREATE TABLE src (id int PRIMARY KEY, x int)",
                "INSERT INTO src VALUES (1,10)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = a FROM src WHERE t_seq.id = src.id",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "join swap",
            set_up_script: &[
                "CREATE TABLE t_seq (id int PRIMARY KEY, a int, b int)",
                "INSERT INTO t_seq VALUES (1,1,0)",
                "CREATE TABLE src (id int PRIMARY KEY, x int)",
                "INSERT INTO src VALUES (1,10)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = b, b = a FROM src WHERE t_seq.id = src.id",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "repeated target is rejected",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = a + 1, a = a + 10, b = a",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"multiple assignments to same column "a""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "assignments through foreign key and check handlers",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY)",
                "INSERT INTO parent VALUES (1), (2)",
                "CREATE TABLE t_seq (id int PRIMARY KEY, a int REFERENCES parent(id), b int, CHECK (b < a))",
                "INSERT INTO t_seq VALUES (9, 1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = 2, b = a RETURNING id, a, b",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("9"), T("2"), T("1")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("9"), T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNING reads completed new row",
            set_up_script: &[
                "CREATE TABLE t_seq (a int, b int)",
                "INSERT INTO t_seq VALUES (1, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t_seq SET a = b, b = a RETURNING a, b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t_seq",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_updates_keeping_primary_keys() {
    run_scripts(&[
        ScriptTest {
            name: "updates that keep primary keys",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE up (id INT PRIMARY KEY, u INT UNIQUE, v INT, w TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX up_v ON up (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX up_w ON up (lower(w));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up SELECT i, i, i % 3, 'W' || i FROM generate_series(1, 20) i;",
                    expected: Expected::Tag("INSERT 0 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET w = w || 'x' WHERE id <= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET v = v + 10 WHERE id BETWEEN 3 AND 8;",
                    expected: Expected::Tag("UPDATE 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET u = u + 100, v = v WHERE id > 15;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET u = u + 1000 WHERE id <= 20;",
                    expected: Expected::Tag("UPDATE 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u, v, w FROM up ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4), Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("1"), T("1001"), T("1"), T("W1x")],
                            &[T("2"), T("1002"), T("2"), T("W2x")],
                            &[T("3"), T("1003"), T("10"), T("W3x")],
                            &[T("4"), T("1004"), T("11"), T("W4x")],
                            &[T("5"), T("1005"), T("12"), T("W5x")],
                            &[T("6"), T("1006"), T("10"), T("W6")],
                            &[T("7"), T("1007"), T("11"), T("W7")],
                            &[T("8"), T("1008"), T("12"), T("W8")],
                            &[T("9"), T("1009"), T("0"), T("W9")],
                            &[T("10"), T("1010"), T("1"), T("W10")],
                            &[T("11"), T("1011"), T("2"), T("W11")],
                            &[T("12"), T("1012"), T("0"), T("W12")],
                            &[T("13"), T("1013"), T("1"), T("W13")],
                            &[T("14"), T("1014"), T("2"), T("W14")],
                            &[T("15"), T("1015"), T("0"), T("W15")],
                            &[T("16"), T("1116"), T("1"), T("W16")],
                            &[T("17"), T("1117"), T("2"), T("W17")],
                            &[T("18"), T("1118"), T("0"), T("W18")],
                            &[T("19"), T("1119"), T("1"), T("W19")],
                            &[T("20"), T("1120"), T("2"), T("W20")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM up WHERE v = 11 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM up WHERE lower(w) = 'w2x' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM up WHERE u = 1003;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET u = 1003 WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "up_u_key""#, detail: "Key (u)=(1003) already exists.", schema: "public", table: "up", constraint: "up_u_key", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE up SET id = id + 100 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u FROM up WHERE id > 100 OR u = 1001 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("1001")],
                            &[T("102"), T("1002")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_update_from_joins() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE FROM and DELETE USING joins",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE uf (id INT PRIMARY KEY, k INT, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX uf_k ON uf (k);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ufs (id INT PRIMARY KEY, k INT, d INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ufl (k INT, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uf SELECT i, i % 10, 0 FROM generate_series(1, 200) i;",
                    expected: Expected::Tag("INSERT 0 200"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ufs SELECT i, i % 20, i FROM generate_series(1, 100) i;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ufl SELECT i % 5, i FROM generate_series(1, 30) i;",
                    expected: Expected::Tag("INSERT 0 30"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uf SET v = s.k FROM ufs s WHERE s.k = uf.k AND s.id <= 10 AND uf.id > 150;",
                    expected: Expected::Tag("UPDATE 45"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, count(*), sum(v) FROM uf GROUP BY k ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("20"), T("0")],
                            &[T("1"), T("20"), T("5")],
                            &[T("2"), T("20"), T("10")],
                            &[T("3"), T("20"), T("15")],
                            &[T("4"), T("20"), T("20")],
                            &[T("5"), T("20"), T("25")],
                            &[T("6"), T("20"), T("30")],
                            &[T("7"), T("20"), T("35")],
                            &[T("8"), T("20"), T("40")],
                            &[T("9"), T("20"), T("45")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uf SET v = t.n FROM (SELECT k, count(*) AS n FROM ufs GROUP BY k) t WHERE t.k = uf.k + 10;",
                    expected: Expected::Tag("UPDATE 200"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, count(*), sum(v) FROM uf GROUP BY k ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("20"), T("100")],
                            &[T("1"), T("20"), T("100")],
                            &[T("2"), T("20"), T("100")],
                            &[T("3"), T("20"), T("100")],
                            &[T("4"), T("20"), T("100")],
                            &[T("5"), T("20"), T("100")],
                            &[T("6"), T("20"), T("100")],
                            &[T("7"), T("20"), T("100")],
                            &[T("8"), T("20"), T("100")],
                            &[T("9"), T("20"), T("100")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uf SET v = uf.v + 1 FROM ufs s, ufl l WHERE s.id = uf.id AND l.v = s.d AND l.k = 2;",
                    expected: Expected::Tag("UPDATE 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM uf WHERE id <= 30 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("6")],
                            &[T("3"), T("5")],
                            &[T("4"), T("5")],
                            &[T("5"), T("5")],
                            &[T("6"), T("5")],
                            &[T("7"), T("6")],
                            &[T("8"), T("5")],
                            &[T("9"), T("5")],
                            &[T("10"), T("5")],
                            &[T("11"), T("5")],
                            &[T("12"), T("6")],
                            &[T("13"), T("5")],
                            &[T("14"), T("5")],
                            &[T("15"), T("5")],
                            &[T("16"), T("5")],
                            &[T("17"), T("6")],
                            &[T("18"), T("5")],
                            &[T("19"), T("5")],
                            &[T("20"), T("5")],
                            &[T("21"), T("5")],
                            &[T("22"), T("6")],
                            &[T("23"), T("5")],
                            &[T("24"), T("5")],
                            &[T("25"), T("5")],
                            &[T("26"), T("5")],
                            &[T("27"), T("6")],
                            &[T("28"), T("5")],
                            &[T("29"), T("5")],
                            &[T("30"), T("5")],
                        ],
                        tag: "SELECT 30",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uf AS a SET v = b.id FROM uf AS b WHERE b.id = a.id + 100 AND a.k = 3;",
                    expected: Expected::Tag("UPDATE 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM uf WHERE k = 3 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("103")],
                            &[T("13"), T("113")],
                            &[T("23"), T("123")],
                            &[T("33"), T("133")],
                            &[T("43"), T("143")],
                            &[T("53"), T("153")],
                            &[T("63"), T("163")],
                            &[T("73"), T("173")],
                            &[T("83"), T("183")],
                            &[T("93"), T("193")],
                            &[T("103"), T("5")],
                            &[T("113"), T("5")],
                            &[T("123"), T("5")],
                            &[T("133"), T("5")],
                            &[T("143"), T("5")],
                            &[T("153"), T("5")],
                            &[T("163"), T("5")],
                            &[T("173"), T("5")],
                            &[T("183"), T("5")],
                            &[T("193"), T("5")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM uf USING ufs s WHERE s.d = uf.id AND s.k > 15;",
                    expected: Expected::Tag("DELETE 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM uf;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("180"), T("18950")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ufl USING uf WHERE uf.k = ufl.k AND uf.id = 7;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, count(*) FROM ufl GROUP BY k ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("6")],
                            &[T("1"), T("6")],
                            &[T("2"), T("6")],
                            &[T("3"), T("6")],
                            &[T("4"), T("6")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ufl SET v = -ufl.v FROM ufs s WHERE s.id = ufl.v AND s.k = 4;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM ufl ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("-24")],
                            &[T("-4")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("14")],
                            &[T("15")],
                            &[T("16")],
                            &[T("17")],
                            &[T("18")],
                            &[T("19")],
                            &[T("20")],
                            &[T("21")],
                            &[T("22")],
                            &[T("23")],
                            &[T("25")],
                            &[T("26")],
                            &[T("27")],
                            &[T("28")],
                            &[T("29")],
                            &[T("30")],
                        ],
                        tag: "SELECT 30",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dml_subqueries_over_the_target() {
    run_scripts(&[
        ScriptTest {
            name: "DML filters with subqueries over the target table",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ds (id INT PRIMARY KEY, v INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dk (v INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ds SELECT i, i % 7, 't' || (i % 4) FROM generate_series(1, 60) i;",
                    expected: Expected::Tag("INSERT 0 60"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dk SELECT i % 5, 'k' || i FROM generate_series(1, 12) i;",
                    expected: Expected::Tag("INSERT 0 12"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ds WHERE id IN (SELECT id FROM ds WHERE v = 3);",
                    expected: Expected::Tag("DELETE 9"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM ds;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("51"), T("1551")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ds SET v = v + 100 WHERE id IN (SELECT max(id) FROM ds GROUP BY t);",
                    expected: Expected::Tag("UPDATE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM ds WHERE v >= 100 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("55"), T("106")],
                            &[T("57"), T("101")],
                            &[T("58"), T("102")],
                            &[T("60"), T("104")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ds SET v = (SELECT max(v) FROM ds) WHERE id < 5;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM ds WHERE id < 8 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("106")],
                            &[T("2"), T("106")],
                            &[T("4"), T("106")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("7"), T("0")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ds WHERE v NOT IN (SELECT v FROM dk) AND v < 100;",
                    expected: Expected::Tag("DELETE 15"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(v) FROM ds;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("36"), T("780")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dk WHERE v IN (SELECT v FROM dk WHERE t > 'k5') OR EXISTS (SELECT 1 FROM ds WHERE ds.id = dk.v);",
                    expected: Expected::Tag("DELETE 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, t FROM dk ORDER BY t;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("0"), T("k10")],
                            &[T("0"), T("k5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dk SET t = t || '!' WHERE v = ANY (SELECT v FROM dk);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, t FROM dk ORDER BY t;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("0"), T("k10!")],
                            &[T("0"), T("k5!")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_planned_updates_and_deletes() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE and DELETE rows found by joins, subqueries, and filters",
            set_up_script: &[
                "CREATE TABLE d1 (id INT PRIMARY KEY, v INT, t TEXT);",
                "CREATE TABLE d2 (id INT PRIMARY KEY, w INT);",
                "INSERT INTO d1 SELECT g, g % 10, 'x' || g FROM generate_series(1, 100) g;",
                "INSERT INTO d2 SELECT g, g * 2 FROM generate_series(1, 50) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE d1 SET v = v + 100 WHERE id < 5 RETURNING id, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                            &[T("2"), T("102")],
                            &[T("3"), T("103")],
                            &[T("4"), T("104")],
                        ],
                        tag: "UPDATE 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d1 SET v = d2.w FROM d2 WHERE d1.id = d2.id AND d2.w > 90 RETURNING d1.id, d1.v, d2.w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("w", INT4)],
                        rows: &[
                            &[T("46"), T("92"), T("92")],
                            &[T("47"), T("94"), T("94")],
                            &[T("48"), T("96"), T("96")],
                            &[T("49"), T("98"), T("98")],
                            &[T("50"), T("100"), T("100")],
                        ],
                        tag: "UPDATE 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM d1 WHERE id IN (SELECT id FROM d2 WHERE w > 96) RETURNING id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("49")],
                            &[T("50")],
                        ],
                        tag: "DELETE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM d1 USING d2 WHERE d1.id = d2.id + 60 AND d2.w < 6 RETURNING d1.id, d2.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("61"), T("1")],
                            &[T("62"), T("2")],
                        ],
                        tag: "DELETE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d1 SET t = 'y' WHERE EXISTS (SELECT 1 FROM d2 WHERE d2.id = d1.id AND d2.w = 20);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM d1 WHERE t = 'y' OR v > 99 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("101"), T("x1")],
                            &[T("2"), T("102"), T("x2")],
                            &[T("3"), T("103"), T("x3")],
                            &[T("4"), T("104"), T("x4")],
                            &[T("10"), T("0"), T("y")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM d1;",
                    expected: Expected::Tag("DELETE 96"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM d1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_returning_old_and_new() {
    run_scripts(&[
        ScriptTest {
            name: "RETURNING the rows before and after the change with OLD and NEW",
            set_up_script: &[
                "CREATE TABLE rt (id INT PRIMARY KEY, v INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (1, 10), (2, 20) RETURNING old.v, new.v, v, old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("v", INT4), Column("v", INT4), Column("id", INT4), Column("v", INT4), Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[Null, T("10"), T("10"), Null, Null, T("1"), T("10")],
                            &[Null, T("20"), T("20"), Null, Null, T("2"), T("20")],
                        ],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = v + 1 RETURNING old.v AS ov, new.v AS nv, rt.v, v;",
                    expected: Expected::Rows {
                        columns: &[Column("ov", INT4), Column("nv", INT4), Column("v", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("10"), T("11"), T("11"), T("11")],
                            &[T("20"), T("21"), T("21"), T("21")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rt WHERE id = 2 RETURNING old.*, new.*, *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("id", INT4), Column("v", INT4), Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("21"), Null, Null, T("2"), T("21")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (1, 99) ON CONFLICT (id) DO UPDATE SET v = excluded.v RETURNING old.v, new.v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("11"), T("99")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = 0 RETURNING WITH (OLD AS o, NEW AS n) o.v, n.v, (SELECT o.v + 1);",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("v", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("99"), T("0"), T("100")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = 1 RETURNING WITH (OLD AS o) old.v;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "missing FROM-clause entry for table \"old\"", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt AS old SET v = 2 RETURNING old.v, new.v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = 3 RETURNING WITH (OLD AS o, OLD AS p) o.v;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "OLD cannot be specified multiple times", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = 4 RETURNING WITH (OLD AS rt) rt.v;",
                    expected: Expected::Error(Diagnostic { code: "42712", message: "table name \"rt\" specified more than once", position: 37, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rt SET v = 5 RETURNING WITH (NEW AS n) n.*;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNING's OLD and NEW rows of plain tables, views, partitions, triggers, and generated columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ro (a INT PRIMARY KEY, b INT, c TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ro VALUES (1, 10, 'x'), (2, 20, 'y') RETURNING old.*, new.*, old, new;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("old", USER_DEFINED), Column("new", USER_DEFINED)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("10"), T("x"), Null, T("(1,10,x)")],
                            &[Null, Null, Null, T("2"), T("20"), T("y"), Null, T("(2,20,y)")],
                        ],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ro SET b = b + 1 WHERE a = 1 RETURNING old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x"), T("1"), T("11"), T("x")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ro SET a = a + 10, c = upper(c) RETURNING old.a, new.a, old.c, new.c, old, new, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4), Column("c", TEXT), Column("c", TEXT), Column("old", USER_DEFINED), Column("new", USER_DEFINED), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("12"), T("y"), T("Y"), T("(2,20,y)"), T("(12,20,Y)"), T("20")],
                            &[T("1"), T("11"), T("x"), T("X"), T("(1,11,x)"), T("(11,11,X)"), T("11")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ro WHERE a = 12 RETURNING old.*, new.*, old, new;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("old", USER_DEFINED), Column("new", USER_DEFINED)],
                        rows: &[
                            &[T("12"), T("20"), T("Y"), Null, Null, Null, T("(12,20,Y)"), Null],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ro RETURNING old.a IS NULL AS no_old, new.a IS NULL AS no_new, old.tableoid::regclass, new.tableoid::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("no_old", BOOL), Column("no_new", BOOL), Column("tableoid", REGCLASS), Column("tableoid", REGCLASS)],
                        rows: &[
                            &[T("f"), T("t"), T("ro"), Null],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ro VALUES (3, 30, 'z') RETURNING WITH (OLD AS o, NEW AS n) o.a, n.a, o, n, o.tableoid::regclass, n.tableoid::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4), Column("o", USER_DEFINED), Column("n", USER_DEFINED), Column("tableoid", REGCLASS), Column("tableoid", REGCLASS)],
                        rows: &[
                            &[Null, T("3"), Null, T("(3,30,z)"), Null, T("ro")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ro SET b = 31 RETURNING WITH (NEW AS n) old.b, n.b, b, (SELECT old.b + n.b);",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("b", INT4), Column("b", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("30"), T("31"), T("31"), T("61")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ro SET b = 32 RETURNING old.b < new.b AS grew, coalesce(old.c, 'none') || '->' || new.c AS change;",
                    expected: Expected::Rows {
                        columns: &[Column("grew", BOOL), Column("change", TEXT)],
                        rows: &[
                            &[T("t"), T("z->z")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE ro SET b = 1 RETURNING old.b, new.b, old, b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on public.ro")],
                            &[T("  Output: old.b, new.b, old.*, b")],
                            &[T("  ->  Seq Scan on public.ro")],
                            &[T("        Output: 1, ctid")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM ro RETURNING WITH (OLD AS o, NEW AS n) o.*, n.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on public.ro")],
                            &[T("  Output: o.a, o.b, o.c, n.a")],
                            &[T("  ->  Seq Scan on public.ro")],
                            &[T("        Output: ctid")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rov AS SELECT a, b, b * 2 AS b2 FROM ro;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rov SET b = 40 RETURNING old.b2, new.b2, old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("b2", INT4), Column("b2", INT4), Column("a", INT4), Column("b", INT4), Column("b2", INT4), Column("a", INT4), Column("b", INT4), Column("b2", INT4)],
                        rows: &[
                            &[T("64"), T("80"), T("3"), T("32"), T("64"), T("3"), T("40"), T("80")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rov (a, b) VALUES (4, 41) RETURNING old.b2, new.b2;",
                    expected: Expected::Rows {
                        columns: &[Column("b2", INT4), Column("b2", INT4)],
                        rows: &[
                            &[Null, T("82")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rov WHERE a = 4 RETURNING old.b2, new.b2;",
                    expected: Expected::Rows {
                        columns: &[Column("b2", INT4), Column("b2", INT4)],
                        rows: &[
                            &[T("82"), Null],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ro_bump() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.b := NEW.b + 100; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ro_bump BEFORE UPDATE ON ro FOR EACH ROW EXECUTE FUNCTION ro_bump();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ro SET b = 50 RETURNING old.b, new.b, b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("b", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("40"), T("150"), T("150")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp (a INT, b TEXT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp1 PARTITION OF rp FOR VALUES IN (1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp2 (b TEXT, a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rp ATTACH PARTITION rp2 FOR VALUES IN (2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rp VALUES (1, 'p') RETURNING old.tableoid::regclass, new.tableoid::regclass, old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[Null, T("rp1"), Null, Null, T("1"), T("p")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rp SET a = 2 RETURNING old.tableoid::regclass, old.*, new.tableoid::regclass, new.*, old, new;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("old", USER_DEFINED), Column("new", USER_DEFINED)],
                        rows: &[
                            &[T("rp1"), T("1"), T("p"), T("rp2"), T("2"), T("p"), T("(1,p)"), T("(2,p)")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rp SET b = 'q' RETURNING old.b, new.b, old.tableoid::regclass, new.tableoid::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("b", TEXT), Column("tableoid", REGCLASS), Column("tableoid", REGCLASS)],
                        rows: &[
                            &[T("p"), T("q"), T("rp2"), T("rp2")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rp RETURNING old.tableoid::regclass, old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("rp2"), T("2"), T("q"), Null, Null],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rg (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL, c INT GENERATED ALWAYS AS (a * 3) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rg (a) VALUES (1) RETURNING old.b, new.b, old.c, new.c;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("b", INT4), Column("c", INT4), Column("c", INT4)],
                        rows: &[
                            &[Null, T("2"), Null, T("3")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rg SET a = 2 RETURNING old.b, new.b, old.c, new.c, old, new;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("b", INT4), Column("c", INT4), Column("c", INT4), Column("old", USER_DEFINED), Column("new", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("4"), T("3"), T("6"), T("(1,2,3)"), T("(2,4,6)")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rg RETURNING old.b, new.b, old.c, new.c;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("b", INT4), Column("c", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("4"), Null, T("6"), Null],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_concurrent_read_committed_changes() {
    run_scripts(&[
        ScriptTest {
            name: "READ COMMITTED changes of a row another transaction changed recheck its newest version",
            set_up_script: &[
                "CREATE TABLE cu (id INT PRIMARY KEY, k INT)",
                "INSERT INTO cu VALUES (1, 0), (2, 0), (3, 0)",
                "CREATE TABLE seen (k INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cu SET k = k + 1 WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE cu SET k = k + 10 WHERE id = 1",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cu ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("0")],
                            &[T("3"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cu SET k = 5 WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE cu SET k = k + 1 WHERE id = 2 AND k = 0",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cu WHERE id = 2",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("2"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM cu WHERE id = 3",
                    expected: Expected::Tag("DELETE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE cu SET k = k + 1 WHERE id = 3",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cu ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cu SET k = 7 WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM cu WHERE id = 2 AND k = 5",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cu ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cu SET k = 8 WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT k FROM cu WHERE id = 2 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM seen",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "An UPDATE whose join matches a row twice changes it once",
            set_up_script: &[
                "CREATE TABLE sm (a INT PRIMARY KEY, b INT)",
                "INSERT INTO sm VALUES (1, 0), (2, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE sm SET b = sm.b + s.x FROM (VALUES (1), (2)) s(x) WHERE sm.a = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sm ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_concurrent_changes_follow_moved_rows() {
    run_scripts(&[
        ScriptTest {
            name: "READ COMMITTED locks and changes follow a row an update moved to another key",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b TEXT)",
                "INSERT INTO p VALUES (1, 'a'), (2, 'b')",
                "CREATE TABLE seen (s TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE p SET a = 10 WHERE a = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT a || b FROM p FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen ORDER BY s",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[
                            &[T("10a")],
                            &[T("2b")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE p SET a = 11 WHERE a = 10",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE p SET b = b || 'x' WHERE b = 'a'",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM p ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("11"), T("ax")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a lock follows the row it saw when the row moved and its old key was used again",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b TEXT)",
                "INSERT INTO p VALUES (1, 'a'), (2, 'b')",
                "CREATE TABLE seen (s TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE p SET a = 20 WHERE a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO p VALUES (2, 'new')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT a || b FROM p FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen ORDER BY s",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[
                            &[T("1a")],
                            &[T("20b")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "READ COMMITTED changes and locks of rows of a table without a primary key follow their updates",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b')",
                "CREATE TABLE seen (s TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET b = 'c' WHERE a = 1",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE k SET b = b || 'y'",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM k ORDER BY a, b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("cy")],
                            &[T("1"), T("cy")],
                            &[T("2"), T("by")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET b = 'd' WHERE a = 1",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT a || b FROM k FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen ORDER BY s",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[
                            &[T("1d")],
                            &[T("1d")],
                            &[T("2by")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPEATABLE READ fails to change a row another transaction moved to another key",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b TEXT)",
                "INSERT INTO p VALUES (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM p",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE p SET a = 30 WHERE a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE p SET b = 'w' WHERE a = 2",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM p ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("30"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_concurrent_changes_of_copies_of_rows() {
    run_scripts(&[
        ScriptTest {
            name: "INSERTs of equal rows into a table without a primary key never wait for each other",
            set_up_script: &[
                "CREATE TABLE u (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO u VALUES (9)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO u VALUES (9)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM u",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM u",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM u",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPEATABLE READ deletes the copy it saw when an equal row was inserted after its snapshot",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO k VALUES (1, 'a')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM k WHERE a = 1",
                    expected: Expected::Tag("DELETE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "row locks on the copies of a row block a DELETE of them until they end",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k WHERE a = 1 FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM k",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "READ COMMITTED skips copies a concurrent DELETE removed and deletes those a rolled back DELETE kept",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM k WHERE a = 1",
                    expected: Expected::Tag("DELETE 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM k",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b')",
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM k WHERE a = 1",
                    expected: Expected::Tag("DELETE 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM k",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPEATABLE READ fails to delete copies a concurrent DELETE removed",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM k WHERE a = 1",
                    expected: Expected::Tag("DELETE 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM k",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent delete", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "count(*) of a table without a primary key counts the copies each snapshot sees",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO k VALUES (1, 'a'), (1, 'a')",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM k WHERE a = 2",
                    expected: Expected::Tag("DELETE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT a, count(*) FROM k GROUP BY a ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_row_locks_taken_twice() {
    run_scripts(&[
        ScriptTest {
            name: "READ COMMITTED locks the same row twice and then updates it",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET b = 'c'",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPEATABLE READ locks the same row twice and then updates it",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET b = 'c'",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a transaction that locks one copy of a row twice leaves the other copy to another transaction",
            set_up_script: &[
                "CREATE TABLE k (a INT, b TEXT)",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k LIMIT 1 FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM k LIMIT 1 FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM k FOR UPDATE SKIP LOCKED",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_rows_a_statement_already_changed() {
    run_scripts(&[
        ScriptTest {
            name: "joined changes of a row twice, keyless copies, REPEATABLE READ, and triggers' earlier changes",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sm (a INT PRIMARY KEY, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sm VALUES (1, 0), (2, 0);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE sm SET b = sm.b + s.x FROM (VALUES (1), (2)) s(x) WHERE sm.a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sm ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM sm USING (VALUES (1), (2)) s(x) WHERE sm.a = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sm ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sm VALUES (3, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM sm USING (VALUES (1), (2)) s(x) WHERE sm.a = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sm ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kl (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (1, 0), (1, 0), (2, 0);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE kl SET b = kl.b + 1 FROM (VALUES (1), (1)) s(x) WHERE kl.a = s.x;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM kl ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM kl USING (VALUES (1), (1)) s(x) WHERE kl.a = s.x;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM kl ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tr (a INT PRIMARY KEY, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (1, 0), (2, 0);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION tr_f() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN UPDATE tr SET b = 99 WHERE a = 2; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr_t BEFORE UPDATE ON tr FOR EACH ROW WHEN (OLD.a = 1) EXECUTE FUNCTION tr_f();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tr SET b = 5;",
                    expected: Expected::Error(Diagnostic { code: "27000", message: "tuple to be updated was already modified by an operation triggered by the current command", hint: "Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tr ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION tr_g() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN DELETE FROM tr WHERE a = 2; RETURN OLD; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr_d BEFORE DELETE ON tr FOR EACH ROW WHEN (OLD.a = 1) EXECUTE FUNCTION tr_g();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM tr;",
                    expected: Expected::Error(Diagnostic { code: "27000", message: "tuple to be updated was already modified by an operation triggered by the current command", hint: "Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tr ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lr (a INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lr VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lr SET a = a;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM lr FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a joined change of a row another transaction changes first, rechecked once, and REPEATABLE READ's conflict",
            set_up_script: &[
                "CREATE TABLE sm2 (a INT PRIMARY KEY, b INT)",
                "INSERT INTO sm2 VALUES (1, 0), (2, 0)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE sm2 SET b = 1 WHERE a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE sm2 SET b = sm2.b + 10 FROM (VALUES (1), (2)) s(x) WHERE sm2.a = 2",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM sm2 ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM sm2",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM sm2 WHERE a = 2",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM sm2 USING (VALUES (1), (2)) s(x) WHERE sm2.a = 2",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent delete", ..E }),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_nested_rechecks() {
    run_scripts(&[
        ScriptTest {
            name: "A READ COMMITTED recheck runs a subquery whose FOR UPDATE rechecks a row of its own",
            set_up_script: &[
                "CREATE TABLE accounts (accountid TEXT PRIMARY KEY, balance NUMERIC NOT NULL)",
                "INSERT INTO accounts VALUES ('checking', 600), ('savings', 600)",
                "CREATE TABLE accounts_ext (accountid TEXT PRIMARY KEY, balance NUMERIC NOT NULL)",
                "INSERT INTO accounts_ext VALUES ('checking', 600), ('savings', 700)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking'",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE accounts_ext SET balance = balance - 200 WHERE accountid = 'checking'",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE accounts SET balance = balance - 1200 WHERE accountid = 'checking' AND balance > 200.0 AND EXISTS (SELECT accountid FROM accounts_ext ae WHERE ae.accountid = accounts.accountid AND ae.balance > 200.0 FOR UPDATE)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM accounts ORDER BY accountid",
                    expected: Expected::Rows {
                        columns: &[Column("accountid", TEXT), Column("balance", NUMERIC)],
                        rows: &[
                            &[T("checking"), T("-800")],
                            &[T("savings"), T("600")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_returning_system_columns_of_rows_routed_to_partitions() {
    run_scripts(&[
        ScriptTest {
            name: "RETURNING reads xmin of a routed row only from a partition whose columns differ from the partitioned table's",
            set_up_script: &[
                "CREATE TABLE utrtest (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE utr1 (a INT CHECK (a IN (1)), q TEXT, b TEXT);",
                "CREATE TABLE utr2 (a INT CHECK (a IN (2)), b TEXT);",
                "ALTER TABLE utr1 DROP COLUMN q;",
                "ALTER TABLE utrtest ATTACH PARTITION utr1 FOR VALUES IN (1);",
                "ALTER TABLE utrtest ATTACH PARTITION utr2 FOR VALUES IN (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO utrtest VALUES (1, 'foo') RETURNING *, tableoid::regclass, xmin = pg_current_xact_id()::xid AS xmin_ok;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("tableoid", REGCLASS), Column("xmin_ok", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), T("utr1"), T("t")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO utrtest VALUES (2, 'bar') RETURNING *, tableoid::regclass, xmin = pg_current_xact_id()::xid AS xmin_ok;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot retrieve a system column in this context", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO utrtest VALUES (2, 'bar') RETURNING *, tableoid::regclass, ctid IS NOT NULL AS has_ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("tableoid", REGCLASS), Column("has_ctid", BOOL)],
                        rows: &[
                            &[T("2"), T("bar"), T("utr2"), T("t")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE utrtest SET a = 3 - a FROM (VALUES (1), (2)) s(x) WHERE a = s.x RETURNING *, tableoid::regclass, xmin = pg_current_xact_id()::xid AS xmin_ok;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot retrieve a system column in this context", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE utrtest SET a = 3 - a FROM (VALUES (1), (2)) s(x) WHERE a = s.x RETURNING *, tableoid::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("x", INT4), Column("tableoid", REGCLASS)],
                        rows: &[
                            &[T("2"), T("foo"), T("1"), T("utr2")],
                            &[T("1"), T("bar"), T("2"), T("utr1")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_update_by_ctid_rechecks_the_ctid_of_a_concurrently_updated_row() {
    run_scripts(&[
        ScriptTest {
            name: "an UPDATE by ctid skips a row a concurrent UPDATE gave another ctid",
            set_up_script: &[
                "CREATE TABLE accounts (accountid TEXT PRIMARY KEY, balance NUMERIC NOT NULL);",
                "INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE accounts SET balance = balance + 100 WHERE ctid = '(0,1)';",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE accounts SET balance = balance + 200 WHERE ctid = '(0,1)';",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT accountid, balance FROM accounts ORDER BY accountid;",
                    expected: Expected::Rows {
                        columns: &[Column("accountid", TEXT), Column("balance", NUMERIC)],
                        rows: &[&[T("checking"), T("700")], &[T("savings"), T("600")]],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an UPDATE by a ctid range skips a row a concurrent UPDATE moved out of the range",
            set_up_script: &[
                "CREATE TABLE accounts (accountid TEXT PRIMARY KEY, balance NUMERIC NOT NULL);",
                "INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE accounts SET balance = balance + 100 WHERE ctid BETWEEN '(0,1)' AND '(0,1)';",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE accounts SET balance = balance + 200 WHERE ctid BETWEEN '(0,1)' AND '(0,1)';",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT accountid, balance FROM accounts ORDER BY accountid;",
                    expected: Expected::Rows {
                        columns: &[Column("accountid", TEXT), Column("balance", NUMERIC)],
                        rows: &[&[T("checking"), T("700")], &[T("savings"), T("600")]],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_update_skips_a_row_deleted_while_another_row_took_its_key() {
    run_scripts(&[
        ScriptTest {
            name: "an UPDATE with a BEFORE trigger that waited for a row deleted meanwhile leaves alone the row another UPDATE moved to its key",
            set_up_script: &[
                "CREATE TABLE trigtest (key TEXT PRIMARY KEY, data TEXT);",
                "INSERT INTO trigtest VALUES ('key-a', 'val-a'), ('key-b', 'val-b');",
                "CREATE FUNCTION trigtest_keep() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;",
                "CREATE TRIGGER trigtest_b_u BEFORE UPDATE ON trigtest FOR EACH ROW EXECUTE FUNCTION trigtest_keep();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE trigtest SET data = data || '-ups1' WHERE key = 'key-b';",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE trigtest SET data = data || '-ups2' WHERE key = 'key-b';",
                    expected: Expected::Tag("UPDATE 0"),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM trigtest WHERE key = 'key-b';",
                    expected: Expected::Tag("DELETE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE trigtest SET key = 'key-b', data = data || '-tobs1' WHERE key = 'key-a';",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT key, data FROM trigtest ORDER BY key;",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("data", TEXT)],
                        rows: &[&[T("key-b"), T("val-a-tobs1")]],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_read_committed_writes_follow_another_transactions_update_of_a_row_of_a_deferrable_key() {
    run_scripts(&[
        ScriptTest {
            name: "an update that changes a deferrable key's value waits for another's update of the row and changes its new version",
            set_up_script: &[
                "CREATE TABLE d (k INT4 PRIMARY KEY DEFERRABLE, v INT4);",
                "INSERT INTO d VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET v = 20 WHERE k = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET k = k + 10 WHERE k = 2 RETURNING k, v;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("12"), T("20")]], tag: "UPDATE 1" },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM d ORDER BY k;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")], &[T("12"), T("20")]], tag: "SELECT 2" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an update of a row of a deferrable key waits for another's update of the row and updates its new version",
            set_up_script: &[
                "CREATE TABLE d (k INT4 PRIMARY KEY DEFERRABLE, v INT4);",
                "INSERT INTO d VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET v = 20 WHERE k = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET v = v + 1 WHERE k = 2 RETURNING k, v;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("2"), T("21")]], tag: "UPDATE 1" },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM d ORDER BY k;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")], &[T("2"), T("21")]], tag: "SELECT 2" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a delete of a row of a deferrable key waits for another's update of the row and deletes its new version",
            set_up_script: &[
                "CREATE TABLE d (k INT4 PRIMARY KEY DEFERRABLE, v INT4);",
                "INSERT INTO d VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET v = 20 WHERE k = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM d WHERE k = 2 RETURNING k, v;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("2"), T("20")]], tag: "DELETE 1" },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM d ORDER BY k;",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_an_update_that_waited_places_its_new_row_after_the_wait() {
    run_scripts(&[
        ScriptTest {
            name: "an update of a table with a primary key that waited for another transaction's update places its new row after that update's",
            set_up_script: &[
                "CREATE TABLE ue (id INTEGER PRIMARY KEY, k INTEGER);",
                "INSERT INTO ue VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ue SET k = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ue SET k = k + 10 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ue VALUES (2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM ue ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("12")],
                            &[T("(0,4)"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an update of a table without a primary key that waited for another transaction's update places its new row after that update's",
            set_up_script: &[
                "CREATE TABLE uk (id INTEGER, k INTEGER);",
                "INSERT INTO uk VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uk SET k = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uk SET k = k + 10 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uk VALUES (2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM uk ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("12")],
                            &[T("(0,4)"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an update of a table with a deferrable primary key that waited for another transaction's update places its new row after that update's",
            set_up_script: &[
                "CREATE TABLE ud (id INTEGER PRIMARY KEY DEFERRABLE, k INTEGER);",
                "INSERT INTO ud VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ud SET k = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ud SET k = k + 10 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ud VALUES (2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM ud ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("12")],
                            &[T("(0,4)"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an update of a table whose primary key the update changes that waited for another transaction's update places its new row after that update's",
            set_up_script: &[
                "CREATE TABLE um (id INTEGER PRIMARY KEY, k INTEGER);",
                "INSERT INTO um VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE um SET k = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE um SET id = id + 10 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO um VALUES (2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM um ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,4)"), T("2"), T("0")],
                            &[T("(0,3)"), T("11"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an ON CONFLICT DO UPDATE that waited for another transaction's insert of the key places its new row after that insert's",
            set_up_script: &[
                "CREATE TABLE oc (id INTEGER PRIMARY KEY, k INTEGER);",
                "INSERT INTO oc VALUES (0, 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (1, 5) ON CONFLICT (id) DO UPDATE SET k = oc.k + 10;",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM oc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("0"), T("0")],
                            &[T("(0,3)"), T("1"), T("11")],
                            &[T("(0,4)"), T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an ON CONFLICT DO NOTHING that waited for another transaction's insert of the key places no row",
            set_up_script: &[
                "CREATE TABLE oc (id INTEGER PRIMARY KEY, k INTEGER);",
                "INSERT INTO oc VALUES (0, 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (1, 5) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM oc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("0"), T("0")],
                            &[T("(0,2)"), T("1"), T("1")],
                            &[T("(0,3)"), T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
