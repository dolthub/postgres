/* A table-wise merge must still produce a valid inheritance graph. Disjoint
 * edits can otherwise combine a new parent column with an old child layout. */
static const char *
inheritance_invalid(Member *members, int count)
{
	for (int i = 0; i < count; i++)
	{
		JsonbValue	definition,
					parents,
					columns;

		if (strcmp(members[i].kind, "table"))
			continue;
		definition = json_document(member_definition(&members[i]));
		if (!json_get(&definition, "inherits", &parents))
			continue;
		json_get(&definition, "columns", &columns);
		int		   *inherited = palloc0_array(int, Max(json_length(&columns), 1));

		for (int p = 0; p < json_length(&parents); p++)
		{
			Member	   *parent = merge_member(members, count, "table", json_item_text(&parents, p));
			JsonbValue	pd,
						pc;

			if (!parent)
				return members[i].name;
			pd = json_document(member_definition(parent));
			json_get(&pd, "columns", &pc);
			for (int c = 0; c < json_length(&pc); c++)
			{
				JsonbValue	column = json_element(&pc, c);
				char	   *name = json_field_text(&column, "name");
				bool		found = false;

				for (int k = 0; k < json_length(&columns); k++)
				{
					JsonbValue	candidate = json_element(&columns, k);
					const char *fields[] = {"type", "typmod", "collation"};

					if (strcmp(name, json_field_text(&candidate, "name")))
						continue;
					found = true;
					inherited[k]++;
					for (int f = 0; f < lengthof(fields); f++)
					{
						JsonbValue	a,
									b;
						bool		ha = json_get(&column, fields[f], &a),
									hb = json_get(&candidate, fields[f], &b);

						if (ha != hb || (ha && compareJsonbContainers(&JsonbValueToJsonb(&a)->root, &JsonbValueToJsonb(&b)->root) != 0))
							return members[i].name;
					}
				}
				if (!found)
					return members[i].name;
			}
		}
		for (int c = 0; c < json_length(&columns); c++)
		{
			JsonbValue	column = json_element(&columns, c);
			char	   *actual = json_field_text(&column, "inhcount");

			if ((actual ? pg_strtoint32(actual) : 0) != inherited[c])
				return members[i].name;
		}
		/* CHECK and NOT NULL constraints propagate, unlike keys and FKs. */
		{
			JsonbValue	constraints;

			json_get(&definition, "constraints", &constraints);
			for (int p = 0; p < json_length(&parents); p++)
			{
				Member	   *parent = merge_member(members, count, "table", json_item_text(&parents, p));
				JsonbValue	pd = json_document(member_definition(parent)),
							pc;

				json_get(&pd, "constraints", &pc);
				for (int c = 0; c < json_length(&pc); c++)
				{
					JsonbValue	constraint = json_element(&pc, c);
					char	   *sql = json_item_text(&constraint, 1);
					char	   *meta = json_item_json(&constraint, 2);
					bool		found = false;

					if ((strncmp(sql, "CHECK ", 6) && strncmp(sql, "NOT NULL ", 9)) ||
						(meta && json_value(meta, "noinherit") && !strcmp(json_value(meta, "noinherit"), "true")))
						continue;
					for (int k = 0; k < json_length(&constraints); k++)
					{
						JsonbValue	candidate = json_element(&constraints, k);

						if (!strcmp(json_item_text(&candidate, 0), json_item_text(&constraint, 0)) &&
							!strcmp(json_item_text(&candidate, 1), sql))
							found = true;
					}
					if (!found)
						return members[i].name;
				}
			}
		}
		/* An edge that returns to this table creates a cycle. */
		{
			List	   *pending = NIL,
					   *seen = NIL;

			for (int p = 0; p < json_length(&parents); p++)
				pending = lappend(pending, json_item_text(&parents, p));
			while (pending)
			{
				char	   *name = linitial(pending);
				Member	   *parent;
				JsonbValue	pd,
							ancestors;

				pending = list_delete_first(pending);
				if (!strcmp(name, members[i].name))
					return members[i].name;
				if (name_selected(seen, name))
					continue;
				seen = lappend(seen, name);
				parent = merge_member(members, count, "table", name);
				if (!parent)
					return members[i].name;
				pd = json_document(member_definition(parent));
				if (json_get(&pd, "inherits", &ancestors))
					for (int p = 0; p < json_length(&ancestors); p++)
						pending = lappend(pending, json_item_text(&ancestors, p));
			}
		}
	}
	return NULL;
}

/* Keep the target's whole connected component on conflict; picking individual
 * table definitions would leave invalid parent/child layouts. */
static bytea *
merge_inheritance(bytea *base, bytea *ours, bytea *theirs, bytea *merged,
				  List **row_merges, StringInfo conflicts)
{
	int			counts[4];
	Member	   *sides[4] = {database_members(base, &counts[0]), database_members(ours, &counts[1]),
	database_members(theirs, &counts[2]), NULL};
	const char *invalid;

	sides[3] = database_members(merged, &counts[3]);
	while ((invalid = inheritance_invalid(sides[3], counts[3])) != NULL)
	{
		List	   *group = list_make1(pstrdup(invalid));
		int			previous;
		StringInfoData names;

		do
		{
			previous = list_length(group);
			for (int side = 0; side < 4; side++)
				for (int i = 0; i < counts[side]; i++)
				{
					Member	   *member = &sides[side][i];
					JsonbValue	definition,
								parents;

					if (strcmp(member->kind, "table"))
						continue;
					definition = json_document(member_definition(member));
					if (!json_get(&definition, "inherits", &parents))
						continue;
					for (int p = 0; p < json_length(&parents); p++)
					{
						char	   *parent = json_item_text(&parents, p);

						if (name_selected(group, member->name) || name_selected(group, parent))
						{
							if (!name_selected(group, member->name))
								group = lappend(group, member->name);
							if (!name_selected(group, parent))
								group = lappend(group, parent);
						}
					}
				}
		} while (previous != list_length(group));
		record_schema_conflict(conflicts, "table", invalid,
							   conflict_schema(merge_member(sides[0], counts[0], "table", invalid), "table", invalid),
							   conflict_schema(merge_member(sides[1], counts[1], "table", invalid), "table", invalid),
							   conflict_schema(merge_member(sides[2], counts[2], "table", invalid), "table", invalid),
							   "incompatible inheritance graph; target component retained, resolve the component with --ours or --theirs");
		initStringInfo(&names);
		appendStringInfoChar(&names, '[');
		foreach_ptr(char, name, group)
		{
			if (names.len > 1)
				appendStringInfoChar(&names, ',');
			escape_json(&names, name);
		}
		appendStringInfoChar(&names, ']');
		conflicts->data[--conflicts->len] = '\0';
		appendStringInfo(conflicts, ",\"inheritance_group\":%s}", names.data);
		merged = compose_database(merged, ours, group);
		foreach_ptr(RowMerge, row, *row_merges)
			if (name_selected(group, row->table))
			*row_merges = foreach_delete_current(*row_merges, row);
		sides[3] = database_members(merged, &counts[3]);
	}
	return merged;
}
