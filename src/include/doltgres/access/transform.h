/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * A table's pending transforms (access' transform.rs): how a row stored
 * under one layout of a table's fields becomes a row under the next.
 */
#ifndef DOLTGRES_ACCESS_TRANSFORM_H
#define DOLTGRES_ACCESS_TRANSFORM_H

#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"

/* AccessTransformSourceTag is Source's variants. */
typedef enum AccessTransformSourceTag
{
	ACCESS_TRANSFORM_SOURCE_OLD,
	ACCESS_TRANSFORM_SOURCE_MISSING,
} AccessTransformSourceTag;

/*
 * AccessTransformSource is Source: where a field of a transform's new layout
 * comes from, a field of the old layout or a value no stored row holds.
 */
typedef struct AccessTransformSource
{
	AccessTransformSourceTag tag;
	union
	{
		/* old is Old's position of the field in the old layout. */
		size_t		old;

		/*
		 * missing is Missing's value as its stored bytes, a Vec<u8>, or none
		 * for NULL.
		 */
		struct
		{
			bool		has;
			StdVec		bytes;
		}			missing;
	}			u;
} AccessTransformSource;

/* AccessTransformField is Field: its type's length, its storage kind, and where its value comes from. */
typedef struct AccessTransformField
{
	int16		typlen;
	uint8		storage;
	AccessTransformSource source;
} AccessTransformField;

/*
 * AccessTransformTransform is Transform: the new layout's fields, a
 * Vec<Field>, and the tuple length its values move out of line until a row
 * is under.
 */
typedef struct AccessTransformTransform
{
	StdVec		fields;
	uint32		target;
} AccessTransformTransform;

extern void access_transform_Source_Clone_clone(const AccessTransformSource *self, AccessTransformSource *result);
extern bool access_transform_Source_PartialEq_eq(const AccessTransformSource *self,
												 const AccessTransformSource *other);
extern bool access_transform_Source_Debug_fmt(const void *value, StdFormatter *f);
extern void access_transform_Source_drop(AccessTransformSource *self);
extern void access_transform_Field_Clone_clone(const AccessTransformField *self, AccessTransformField *result);
extern bool access_transform_Field_PartialEq_eq(const AccessTransformField *self, const AccessTransformField *other);
extern bool access_transform_Field_Debug_fmt(const void *value, StdFormatter *f);
extern void access_transform_Field_drop(AccessTransformField *self);
extern void access_transform_Transform_Clone_clone(const AccessTransformTransform *self,
												   AccessTransformTransform *result);
extern bool access_transform_Transform_PartialEq_eq(const AccessTransformTransform *self,
													const AccessTransformTransform *other);
extern bool access_transform_Transform_Debug_fmt(const void *value, StdFormatter *f);
extern void access_transform_Transform_drop(AccessTransformTransform *self);
extern bool access_transform_Transform_then(const AccessTransformTransform *self,
											const AccessTransformTransform *later, AccessTransformTransform *result,
											PgPgError *error);
extern void access_transform_Transform_encode(const AccessTransformTransform *self, StdVec *result);
extern bool access_transform_Transform_decode(const uint8 *bytes, size_t bytes_len, AccessTransformTransform *result,
											  PgPgError *error);
extern bool access_transform_compose(const AccessTransformTransform *chain, size_t chain_len, bool *has_result,
									 AccessTransformTransform *result, PgPgError *error);

#ifdef DOLTGRES_TEST_ACCESS
extern void test_access_transform_tests_round_trip(void);
extern void test_access_transform_tests_composed(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_ACCESS_TRANSFORM_H */
