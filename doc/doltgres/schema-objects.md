# Versioned aggregates, operators and casts

Aggregate, operator and cast definitions are database-root members with canonical
schema children. DGSCHEMAOBJECT1 serializes the native catalog fields, translating
object references to qualified names/signatures. Aggregates also contain their
pg_proc definition. Runtime OIDs are supplied by the branch catalog provider;
canonical objects do not contain those allocation-dependent identifiers.

Native CREATE, ALTER, DROP, ownership, comments and dependency handling operate on
provisional catalog tuples. Publication commits the resulting definitions in the
working root. Checkout, three-way merge and native backup carry these members as
ordinary schema objects. Conflicting edits use the existing schema conflict path.
Stored expressions retain their aggregate/operator dependencies, and prepared
expressions revalidate their branch bindings when the working root changes.

This does not introduce versioned operator classes or operator families. Extension
installation remains on the native shared-catalog path. Existing restrictions on
canonical event-trigger DDL and unsupported routine languages still apply.

Validation: test_prolly/046_schema_objects; Doltgres TestCreateAggregate,
TestCreateOperator, TestCasts, TestDoltBackup and
TestPostgresForkVersionedSchemaObjects. The last test includes a reused prepared
statement across branch changes and replacement of its implementation function.
