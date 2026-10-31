/*
 * Rebuild versioned user-defined types. All of a schema's types, and their
 * array types, are bound before any rows are built, as types, tables and
 * routines name one another; arrays are found through |rows->arrays|.
 */
static void
provider_type_bind(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	char	   *array = json_value(definition, "array");
	Oid			type = CatalogViewBindObject(view, TypeRelationId, name, InvalidOid);

	if (array)
	{
		char	   *schema = strVal(linitial(stringToQualifiedNameList(name, NULL)));

		rows->elements = lappend_oid(rows->elements, type);
		rows->arrays = lappend_oid(rows->arrays, CatalogViewBindObject(view, TypeRelationId, dg_qualified_name(schema, array), InvalidOid));
	}
	if (!strcmp(json_value(definition, "kind"), "c"))
		(void) CatalogViewBindObject(view, RelationRelationId, name, InvalidOid);

	/* A range type's multirange type and its array. */
	if (!strcmp(json_value(definition, "kind"), "r"))
	{
		char	   *range = json_value(definition, "range");
		char	   *multirange = json_value(range, "multirange");
		char	   *schema = strVal(linitial(stringToQualifiedNameList(multirange, NULL)));

		rows->elements = lappend_oid(rows->elements, CatalogViewBindObject(view, TypeRelationId, multirange, InvalidOid));
		rows->arrays = lappend_oid(rows->arrays, CatalogViewBindObject(view, TypeRelationId,
																	   dg_qualified_name(schema, json_value(range, "multirange_array")),
																	   InvalidOid));
	}
}

/* The array type of |element| among the types being rebuilt, if any. */
static Oid
provider_array_of(DGProviderRows *rows, Oid element)
{
	ListCell   *e,
			   *a;

	forboth(e, rows->elements, a, rows->arrays)
	{
		if (lfirst_oid(e) == element)
			return lfirst_oid(a);
	}
	return InvalidOid;
}

static Oid
provider_collation(const char *name)
{
	return name ? get_collation_oid(stringToQualifiedNameList(name, NULL), false) : InvalidOid;
}

static void
provider_type_array(DGProviderRows *rows, Form_pg_type type, const char *arrayname)
{
	FormData_pg_type array = {0};

	array.oid = type->typarray;
	namestrcpy(&array.typname, arrayname);
	array.typnamespace = type->typnamespace;
	array.typowner = type->typowner;
	array.typlen = -1;
	array.typtype = TYPTYPE_BASE;
	array.typcategory = TYPCATEGORY_ARRAY;
	array.typisdefined = true;
	array.typdelim = type->typdelim;
	array.typelem = type->oid;
	array.typinput = F_ARRAY_IN;
	array.typoutput = F_ARRAY_OUT;
	array.typreceive = F_ARRAY_RECV;
	array.typsend = F_ARRAY_SEND;
	array.typanalyze = F_ARRAY_TYPANALYZE;
	array.typsubscript = F_ARRAY_SUBSCRIPT_HANDLER;
	array.typalign = type->typalign == TYPALIGN_DOUBLE ? TYPALIGN_DOUBLE : TYPALIGN_INT;
	array.typstorage = TYPSTORAGE_EXTENDED;
	array.typtypmod = -1;
	array.typcollation = type->typcollation;
	provider_add(rows, TypeRelationId, &array, sizeof(array), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, array.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
}

/*
 * A constructor of a range or multirange type |owner|, as makeRangeConstructors
 * and makeMultirangeConstructors make it, named by its signature, whose
 * argument types |argnames| name; a variadic one takes elements of type
 * |variadic|.
 */
static Oid
provider_range_constructor(DGProviderRows *rows, CatalogView *view, const char *schema, const char *name, Oid namespace,
						   Oid owner, int nargs, Oid *args, char **argnames, const char *source, Oid variadic)
{
	int			slot = provider_catalog_index(ProcedureRelationId);
	TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
	Datum		values[Natts_pg_proc] = {0};
	bool		nulls[Natts_pg_proc] = {0};
	StringInfoData identity;
	Oid			oid;

	initStringInfo(&identity);
	appendStringInfo(&identity, "%s(", dg_qualified_name(schema, name));
	for (int i = 0; i < nargs; i++)
		appendStringInfo(&identity, "%s%s", i ? "," : "", argnames[i]);
	appendStringInfoChar(&identity, ')');
	oid = CatalogViewBindObject(view, ProcedureRelationId, identity.data, InvalidOid);
	values[Anum_pg_proc_oid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_proc_proname - 1] = DirectFunctionCall1(namein, CStringGetDatum(name));
	values[Anum_pg_proc_pronamespace - 1] = ObjectIdGetDatum(namespace);
	values[Anum_pg_proc_proowner - 1] = ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID);
	values[Anum_pg_proc_prolang - 1] = ObjectIdGetDatum(INTERNALlanguageId);
	values[Anum_pg_proc_procost - 1] = Float4GetDatum(1);
	values[Anum_pg_proc_prorows - 1] = Float4GetDatum(0);
	values[Anum_pg_proc_provariadic - 1] = ObjectIdGetDatum(variadic);
	values[Anum_pg_proc_prosupport - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_proc_prokind - 1] = CharGetDatum(PROKIND_FUNCTION);
	values[Anum_pg_proc_prosecdef - 1] = BoolGetDatum(false);
	values[Anum_pg_proc_proleakproof - 1] = BoolGetDatum(false);
	/* Range constructors take null bounds; multirange constructors are strict. */
	values[Anum_pg_proc_proisstrict - 1] = BoolGetDatum(strncmp(source, "range_", 6) != 0);
	values[Anum_pg_proc_proretset - 1] = BoolGetDatum(false);
	values[Anum_pg_proc_provolatile - 1] = CharGetDatum(PROVOLATILE_IMMUTABLE);
	values[Anum_pg_proc_proparallel - 1] = CharGetDatum(PROPARALLEL_SAFE);
	values[Anum_pg_proc_pronargs - 1] = Int16GetDatum(nargs);
	values[Anum_pg_proc_pronargdefaults - 1] = Int16GetDatum(0);
	values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_proc_proargtypes - 1] = PointerGetDatum(buildoidvector(args, nargs));
	if (OidIsValid(variadic))
	{
		Datum		all = ObjectIdGetDatum(args[0]);
		Datum		mode = CharGetDatum(FUNC_PARAM_VARIADIC);

		values[Anum_pg_proc_proallargtypes - 1] = PointerGetDatum(construct_array_builtin(&all, 1, OIDOID));
		values[Anum_pg_proc_proargmodes - 1] = PointerGetDatum(construct_array_builtin(&mode, 1, CHAROID));
	}
	else
	{
		nulls[Anum_pg_proc_proallargtypes - 1] = true;
		nulls[Anum_pg_proc_proargmodes - 1] = true;
	}
	nulls[Anum_pg_proc_proargnames - 1] = true;
	nulls[Anum_pg_proc_proargdefaults - 1] = true;
	nulls[Anum_pg_proc_protrftypes - 1] = true;
	values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(source);
	nulls[Anum_pg_proc_probin - 1] = true;
	nulls[Anum_pg_proc_prosqlbody - 1] = true;
	nulls[Anum_pg_proc_proconfig - 1] = true;
	nulls[Anum_pg_proc_proacl - 1] = true;
	rows->rows[slot] = lappend(rows->rows[slot], heap_form_tuple(desc, values, nulls));
	provider_dependency(rows, ProcedureRelationId, oid, 0, NamespaceRelationId, namespace, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, ProcedureRelationId, oid, 0, TypeRelationId, owner, 0, DEPENDENCY_INTERNAL);
	return oid;
}

/*
 * What range type |type| derives, as DefineRange makes it: its multirange
 * type and that type's array, its pg_range row, the constructors of both and
 * the cast between them.
 */
static void
provider_range(DGProviderRows *rows, CatalogView *view, const char *definition, Form_pg_type type)
{
	char	   *name = json_value(definition, "name");
	char	   *schema = strVal(linitial(stringToQualifiedNameList(name, NULL)));
	char	   *range = json_value(definition, "range");
	char	   *multirange_name = json_value(range, "multirange");
	char	   *subtype_name = json_value(range, "subtype");
	char	   *subdiff = json_value(range, "subdiff");
	List	   *parts = stringToQualifiedNameList(multirange_name, NULL);
	FormData_pg_type multirange = {0};
	FormData_pg_range form = {0};
	FormData_pg_cast cast = {0};
	Oid			args[3];
	char	   *argnames[3];

	multirange.oid = CatalogViewFindObject(view, TypeRelationId, multirange_name);
	namestrcpy(&multirange.typname, strVal(lsecond(parts)));
	multirange.typnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	multirange.typowner = type->typowner;
	multirange.typlen = -1;
	multirange.typtype = TYPTYPE_MULTIRANGE;
	multirange.typcategory = TYPCATEGORY_RANGE;
	multirange.typisdefined = true;
	multirange.typdelim = DEFAULT_TYPDELIM;
	multirange.typarray = provider_array_of(rows, multirange.oid);
	multirange.typinput = F_MULTIRANGE_IN;
	multirange.typoutput = F_MULTIRANGE_OUT;
	multirange.typreceive = F_MULTIRANGE_RECV;
	multirange.typsend = F_MULTIRANGE_SEND;
	multirange.typanalyze = F_MULTIRANGE_TYPANALYZE;
	multirange.typalign = type->typalign;
	multirange.typstorage = TYPSTORAGE_EXTENDED;
	multirange.typtypmod = -1;
	provider_add(rows, TypeRelationId, &multirange, sizeof(multirange), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, multirange.oid, 0, NamespaceRelationId, multirange.typnamespace, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, TypeRelationId, multirange.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
	provider_type_array(rows, &multirange, json_value(range, "multirange_array"));

	form.rngtypid = type->oid;
	form.rngsubtype = routine_type_oid(rows, view, subtype_name);
	form.rngmultitypid = multirange.oid;
	form.rngcollation = provider_collation(json_value(range, "collation"));
	form.rngsubopc = get_opclass_oid(BTREE_AM_OID, list_make2(makeString("pg_catalog"), makeString(json_value(range, "opclass"))), false);
	form.rngsubdiff = subdiff ? DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum(subdiff))) : InvalidOid;
	provider_add(rows, RangeRelationId, &form, sizeof(form), Natts_pg_range);
	provider_dependency(rows, TypeRelationId, type->oid, 0, TypeRelationId, form.rngsubtype, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, TypeRelationId, type->oid, 0, OperatorClassRelationId, form.rngsubopc, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(form.rngcollation))
		provider_dependency(rows, TypeRelationId, type->oid, 0, CollationRelationId, form.rngcollation, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(form.rngsubdiff))
		provider_dependency(rows, TypeRelationId, type->oid, 0, ProcedureRelationId, form.rngsubdiff, 0, DEPENDENCY_NORMAL);

	args[0] = args[1] = form.rngsubtype;
	args[2] = TEXTOID;
	argnames[0] = argnames[1] = subtype_name;
	argnames[2] = "pg_catalog.text";
	(void) provider_range_constructor(rows, view, schema, json_value(range, "constructor"), type->typnamespace, type->oid,
									  2, args, argnames, "range_constructor2", InvalidOid);
	(void) provider_range_constructor(rows, view, schema, json_value(range, "constructor"), type->typnamespace, type->oid,
									  3, args, argnames, "range_constructor3", InvalidOid);
	(void) provider_range_constructor(rows, view, schema, json_value(range, "multirange_constructor"), type->typnamespace,
									  multirange.oid, 0, args, argnames, "multirange_constructor0", InvalidOid);
	args[0] = type->oid;
	argnames[0] = name;
	cast.castfunc = provider_range_constructor(rows, view, schema, json_value(range, "multirange_constructor"), type->typnamespace,
											   multirange.oid, 1, args, argnames, "multirange_constructor1", InvalidOid);
	args[0] = type->typarray;
	argnames[0] = psprintf("%s[]", name);
	(void) provider_range_constructor(rows, view, schema, json_value(range, "multirange_constructor"), type->typnamespace,
									  multirange.oid, 1, args, argnames, "multirange_constructor2", type->oid);

	/* As CastCreate: the cast from the range type to its multirange type. */
	cast.oid = CatalogViewBindObject(view, CastRelationId, psprintf("%s/multirange", name), InvalidOid);
	cast.castsource = type->oid;
	cast.casttarget = multirange.oid;
	cast.castcontext = COERCION_CODE_EXPLICIT;
	cast.castmethod = COERCION_METHOD_FUNCTION;
	provider_add(rows, CastRelationId, &cast, sizeof(cast), Natts_pg_cast);
	provider_dependency(rows, CastRelationId, cast.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
	provider_dependency(rows, CastRelationId, cast.oid, 0, TypeRelationId, multirange.oid, 0, DEPENDENCY_INTERNAL);
	provider_dependency(rows, CastRelationId, cast.oid, 0, ProcedureRelationId, cast.castfunc, 0, DEPENDENCY_INTERNAL);
}

static void
provider_type(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	int			slot = provider_catalog_index(TypeRelationId);
	TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Datum		values[Natts_pg_type] = {0};
	bool		nulls[Natts_pg_type] = {0};
	HeapTuple	tuple;
	Form_pg_type type;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		char	   *encoded = json_value(definition, NameStr(attr->attname));
		Oid			input,
					param;

		switch (i + 1)
		{
			case Anum_pg_type_oid:
				values[i] = ObjectIdGetDatum(CatalogViewFindObject(view, TypeRelationId, name));
				continue;
			case Anum_pg_type_typname:
				values[i] = DirectFunctionCall1(namein, CStringGetDatum(strVal(lsecond(parts))));
				continue;
			case Anum_pg_type_typrelid:
				values[i] = ObjectIdGetDatum(!strcmp(json_value(definition, "kind"), "c") ?
											 CatalogViewFindObject(view, RelationRelationId, name) : InvalidOid);
				continue;
			case Anum_pg_type_typarray:
				values[i] = ObjectIdGetDatum(provider_array_of(rows, CatalogViewFindObject(view, TypeRelationId, name)));
				continue;
			case Anum_pg_type_typacl:
			case Anum_pg_type_typdefault:
			case Anum_pg_type_typdefaultbin:
				/* A domain's default is bound with its constraints. */
				nulls[i] = true;
				continue;
		}
		if (!encoded)
		{
			if (attr->atttypid == OIDOID || attr->atttypid == REGPROCOID)
				values[i] = ObjectIdGetDatum(InvalidOid);
			else
				nulls[i] = true;
			continue;
		}
		switch (i + 1)
		{
			case Anum_pg_type_typnamespace:
				values[i] = ObjectIdGetDatum(CatalogViewLookupObject(view, NamespaceRelationId, encoded));
				break;
			case Anum_pg_type_typowner:
				values[i] = ObjectIdGetDatum(provider_role(encoded));
				break;
			case Anum_pg_type_typelem:
			case Anum_pg_type_typbasetype:
				values[i] = ObjectIdGetDatum(routine_type_oid(rows, view, encoded));
				break;
			case Anum_pg_type_typcollation:
				values[i] = ObjectIdGetDatum(provider_collation(encoded));
				break;
			default:
				getTypeInputInfo(attr->atttypid, &input, &param);
				values[i] = OidInputFunctionCall(input, encoded, param, -1);
				break;
		}
	}
	tuple = heap_form_tuple(desc, values, nulls);
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	type = (Form_pg_type) GETSTRUCT(tuple);
	provider_dependency(rows, TypeRelationId, type->oid, 0, NamespaceRelationId, type->typnamespace, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(type->typbasetype))
		provider_dependency(rows, TypeRelationId, type->oid, 0, TypeRelationId, type->typbasetype, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(type->typarray))
		provider_type_array(rows, type, json_value(definition, "array"));
	if (type->typtype == TYPTYPE_RANGE)
		provider_range(rows, view, definition, type);

	if (type->typtype == TYPTYPE_ENUM)
	{
		/* Labels are bound in sort order, so even OIDs compare in label order. */
		int			count;
		JsonbValue *labels = json_elements(definition, "labels", &count);

		for (int j = 0; j < count; j++)
		{
			FormData_pg_enum label = {0};
			char	   *text = json_text(&labels[j]);

			label.oid = CatalogViewBindObject(view, EnumRelationId, psprintf("%s/%s", name, text), InvalidOid);
			label.enumtypid = type->oid;
			label.enumsortorder = j + 1;
			namestrcpy(&label.enumlabel, text);
			provider_add(rows, EnumRelationId, &label, sizeof(label), Anum_pg_enum_enumlabel);
		}
	}
}

/*
 * A composite type's relation and attributes, once every type's row exists:
 * an attribute's type may be any of them.
 */
static void
provider_type_relation(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	Form_pg_type type = provider_row_type(rows, CatalogViewFindObject(view, TypeRelationId, json_value(definition, "name")));

	if (type->typtype == TYPTYPE_COMPOSITE)
	{
		FormData_pg_class form = {0};
		int			natts;
		JsonbValue *attributes;

		form.oid = type->typrelid;
		form.relnamespace = type->typnamespace;
		form.relname = type->typname;
		form.relowner = type->typowner;
		form.reltype = type->oid;
		form.relkind = RELKIND_COMPOSITE_TYPE;
		form.relpersistence = RELPERSISTENCE_PERMANENT;
		form.reltuples = -1;
		form.relispopulated = true;
		form.relreplident = REPLICA_IDENTITY_NOTHING;
		attributes = json_elements(definition, "attributes", &natts);

		form.relnatts = natts;
		{
			char	  **names = palloc_array(char *, Max(natts, 1));
			char	  **types = palloc_array(char *, Max(natts, 1));
			char	  **collations = palloc_array(char *, Max(natts, 1));
			int		   *mods = palloc_array(int, Max(natts, 1));

			for (int j = 0; j < natts; j++)
			{
				names[j] = json_field_text(&attributes[j], "name");
				types[j] = json_field_text(&attributes[j], "type");
				mods[j] = json_int(json_field_text(&attributes[j], "typmod"));
				collations[j] = json_field_text(&attributes[j], "collation");
			}
			provider_add(rows, RelationRelationId, &form, sizeof(form), Anum_pg_class_relminmxid);
			for (int j = 0; j < natts; j++)
			{
				TupleDesc	one = CreateTemplateTupleDesc(1);
				Oid			atttype = routine_type_oid(rows, view, types[j]);
				Form_pg_attribute attr;

				provider_init_entry(one, 1, names[j], atttype, mods[j]);
				attr = TupleDescAttr(one, 0);
				attr->attrelid = form.oid;
				attr->attnum = j + 1;
				attr->attislocal = true;
				if (collations[j])
					attr->attcollation = provider_collation(collations[j]);
				provider_add(rows, AttributeRelationId, attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
				provider_dependency(rows, RelationRelationId, form.oid, j + 1, TypeRelationId, atttype, 0, DEPENDENCY_NORMAL);
			}
		}
		/* As GenerateTypeDependencies: a stand-alone composite's relation belongs to its type. */
		provider_dependency(rows, RelationRelationId, form.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
	}
}

/* Bind a domain's default and constraints, once routines can be named. */
static void
provider_domain(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	char	   *defaultsql = json_value(definition, "typdefaultbin");
	Oid			oid = CatalogViewFindObject(view, TypeRelationId, name);
	int			tslot = provider_catalog_index(TypeRelationId);
	int			cslot = provider_catalog_index(ConstraintRelationId);
	Form_pg_type type = NULL;
	ListCell   *cell_of_type = NULL;
	char	  **names,
			  **defs;
	bool	   *validated;
	JsonbValue *constraints;
	int			n;

	foreach(cell_of_type, rows->rows[tslot])
	{
		type = (Form_pg_type) GETSTRUCT((HeapTuple) lfirst(cell_of_type));
		if (type->oid == oid)
			break;
		type = NULL;
	}
	if (!type)
		elog(ERROR, "canonical domain %s is missing", name);
	if (defaultsql)
	{
		Node	   *expression = schema_expression_bind_domain(view, schema_expression_raw(defaultsql), name,
															   type->typbasetype, type->typtypmod, type->typcollation, false);
		HeapTuple	tuple = (HeapTuple) lfirst(cell_of_type);

		tuple = provider_set(rows->relations[tslot], tuple, Anum_pg_type_typdefaultbin, CStringGetTextDatum(nodeToString(expression)));
		tuple = provider_set(rows->relations[tslot], tuple, Anum_pg_type_typdefault, CStringGetTextDatum(defaultsql));
		lfirst(cell_of_type) = tuple;
		type = (Form_pg_type) GETSTRUCT(tuple);
		provider_expression_dependencies(rows, TypeRelationId, oid, expression);
	}
	constraints = json_elements(definition, "constraints", &n);
	names = palloc_array(char *, Max(n, 1));
	defs = palloc_array(char *, Max(n, 1));
	validated = palloc_array(bool, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		names[i] = json_item_text(&constraints[i], 0);
		defs[i] = json_item_text(&constraints[i], 1);
		validated[i] = json_bool(json_item_text(&constraints[i], 2));
	}
	for (int i = 0; i < n; i++)
	{
		List	   *parsed = schema_expression_parse(psprintf("ALTER DOMAIN placeholder ADD CONSTRAINT %s %s", dg_quote_identifier(names[i]), defs[i]));
		AlterDomainStmt *stmt;
		Constraint *constraint;
		FormData_pg_constraint form = {0};
		Node	   *expression = NULL;
		HeapTuple	tuple;

		if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterDomainStmt))
			elog(ERROR, "invalid canonical domain constraint %s", names[i]);
		stmt = castNode(AlterDomainStmt, linitial_node(RawStmt, parsed)->stmt);
		constraint = castNode(Constraint, stmt->def);
		form.oid = CatalogViewBindObject(view, ConstraintRelationId, psprintf("%s/%s", name, names[i]), InvalidOid);
		namestrcpy(&form.conname, names[i]);
		form.connamespace = type->typnamespace;
		form.contypid = oid;
		form.conenforced = true;
		form.convalidated = validated[i];
		form.conislocal = true;
		form.confupdtype = ' ';
		form.confdeltype = ' ';
		form.confmatchtype = ' ';
		if (constraint->contype == CONSTR_CHECK)
		{
			form.contype = CONSTRAINT_CHECK;
			expression = schema_expression_bind_domain(view, constraint->raw_expr, name, type->typbasetype,
													   type->typtypmod, type->typcollation, true);
		}
		else if (constraint->contype == CONSTR_NOTNULL)
			form.contype = CONSTRAINT_NOTNULL;
		else
			elog(ERROR, "unsupported canonical domain constraint %s", names[i]);
		tuple = provider_fixed(rows->relations[cslot], &form, sizeof(form), Anum_pg_constraint_conperiod);
		if (expression)
			tuple = provider_set(rows->relations[cslot], tuple, Anum_pg_constraint_conbin, CStringGetTextDatum(nodeToString(expression)));
		rows->rows[cslot] = lappend(rows->rows[cslot], tuple);
		provider_dependency(rows, ConstraintRelationId, form.oid, 0, TypeRelationId, oid, 0, DEPENDENCY_AUTO);
		if (expression)
			provider_expression_dependencies(rows, ConstraintRelationId, form.oid, expression);
	}
}
