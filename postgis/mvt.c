/**********************************************************************
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.net
 *
 * PostGIS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * PostGIS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PostGIS.  If not, see <http://www.gnu.org/licenses/>.
 *
 **********************************************************************
 *
 * Copyright (C) 2016-2017 Björn Harrtell <bjorn@wololo.org>
 *
 **********************************************************************/

#include <stdbool.h>
#include <string.h>
#include <float.h>
#include <math.h>

#include "mvt.h"
#include "lwgeom_geos.h"

#ifdef HAVE_LIBPROTOBUF
#include "utils/jsonb.h"

#include "lwgeom_wagyu.h"

#define uthash_fatal(msg) lwerror("uthash: fatal error (out of memory)")
#define uthash_malloc(sz) palloc(sz)
#define uthash_free(ptr,sz) pfree(ptr)
/* Note: set UTHASH_FUNCTION (not HASH_FUNCTION) to change the hash function */
#include "uthash.h"

#include "vector_tile.pb-c.h"

#define FEATURES_CAPACITY_INITIAL 50

enum mvt_cmd_id
{
	CMD_MOVE_TO = 1,
	CMD_LINE_TO = 2,
	CMD_CLOSE_PATH = 7
};

enum mvt_type
{
	MVT_POINT = 1,
	MVT_LINE = 2,
	MVT_RING = 3
};

struct mvt_kv_key
{
	char *name;
	uint32_t id;
	UT_hash_handle hh;
};

struct mvt_kv_value {
	VectorTile__Tile__Value value[1];
	uint32_t id;
	UT_hash_handle hh;
};


/* Must be >= 2, otherwise an overflow will occur at the first grow, as tags come in pairs */
#define TAGS_INITIAL_CAPACITY 20

/* This structure keeps track of the capacity of
 * the tags array while the feature is being built.
 */
struct feature_builder {
	bool has_id;
	uint64_t id;

	/* A growable array of tags */
	size_t n_tags;
	size_t tags_capacity;
	uint32_t *tags;

	/* The geometry of the feature. */
	VectorTile__Tile__GeomType type;
	size_t n_geometry;
	uint32_t *geometry;
};

static void feature_init(struct feature_builder *builder)
{
	builder->has_id = false;
	builder->n_tags = 0;
	builder->tags_capacity = TAGS_INITIAL_CAPACITY;
	builder->tags = palloc(TAGS_INITIAL_CAPACITY * sizeof(*builder->tags));
	builder->type = VECTOR_TILE__TILE__GEOM_TYPE__UNKNOWN;
	builder->n_geometry = 0;
	builder->geometry = NULL;
}

static VectorTile__Tile__Feature *feature_build(struct feature_builder *builder)
{
	VectorTile__Tile__Feature *feature = palloc(sizeof(*feature));
	vector_tile__tile__feature__init(feature);

	feature->has_id = builder->has_id;
	feature->id = builder->id;
	feature->n_tags = builder->n_tags;
	feature->tags = builder->tags;
	feature->type = builder->type;
	feature->n_geometry = builder->n_geometry;
	feature->geometry = builder->geometry;
	return feature;
}

static void feature_add_property(struct feature_builder *builder, uint32_t key_id, uint32_t value_id)
{
	size_t new_n_tags = builder->n_tags + 2;
	if (new_n_tags >= builder->tags_capacity)
	{
		size_t new_capacity = builder->tags_capacity * 2;
		builder->tags = repalloc(builder->tags, new_capacity * sizeof(*builder->tags));
		builder->tags_capacity = new_capacity;
	}

	builder->tags[builder->n_tags] = key_id;
	builder->tags[builder->n_tags + 1] = value_id;
	builder->n_tags = new_n_tags;
}


static inline uint32_t c_int(enum mvt_cmd_id id, uint32_t count)
{
	return (id & 0x7) | (count << 3);
}

static inline uint32_t p_int(int32_t value)
{
	return (value << 1) ^ (value >> 31);
}

static uint32_t encode_ptarray(enum mvt_type type, POINTARRAY *pa, uint32_t *buffer, int32_t *px, int32_t *py)
{
	uint32_t offset = 0;
	uint32_t i, c = 0;
	int32_t dx, dy, x, y;
	const POINT2D *p;

	/* loop points and add to buffer */
	for (i = 0; i < pa->npoints; i++)
	{
		/* move offset for command */
		if (i == 0 || (i == 1 && type > MVT_POINT))
			offset++;
		/* skip closing point for rings */
		if (type == MVT_RING && i == pa->npoints - 1)
			break;
		p = getPoint2d_cp(pa, i);
		x = p->x;
		y = p->y;
		dx = x - *px;
		dy = y - *py;
		buffer[offset++] = p_int(dx);
		buffer[offset++] = p_int(dy);
		*px = x;
		*py = y;
		c++;
	}

	/* determine initial move and eventual line command */
	if (type == MVT_POINT)
	{
		/* point or multipoint, use actual number of point count */
		buffer[0] = c_int(CMD_MOVE_TO, c);
	}
	else
	{
		/* line or polygon, assume count 1 */
		buffer[0] = c_int(CMD_MOVE_TO, 1);
		/* line command with move point subtracted from count */
		buffer[3] = c_int(CMD_LINE_TO, c - 1);
	}

	/* add close command if ring */
	if (type == MVT_RING)
		buffer[offset++] = c_int(CMD_CLOSE_PATH, 1);

	return offset;
}

static uint32_t encode_ptarray_initial(enum mvt_type type, POINTARRAY *pa, uint32_t *buffer)
{
	int32_t px = 0, py = 0;
	return encode_ptarray(type, pa, buffer, &px, &py);
}

static void encode_point(struct feature_builder *feature, LWPOINT *point)
{
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__POINT;
	feature->n_geometry = 3;
	feature->geometry = palloc(sizeof(*feature->geometry) * 3);
	encode_ptarray_initial(MVT_POINT, point->point, feature->geometry);
}

static void encode_mpoint(struct feature_builder *feature, LWMPOINT *mpoint)
{
	size_t c;
	// NOTE: inefficient shortcut LWMPOINT->LWLINE
	LWLINE *lwline = lwline_from_lwmpoint(mpoint->srid, mpoint);
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__POINT;
	c = 1 + lwline->points->npoints * 2;
	feature->geometry = palloc(sizeof(*feature->geometry) * c);
	feature->n_geometry = encode_ptarray_initial(MVT_POINT,
		lwline->points, feature->geometry);
}

static void encode_line(struct feature_builder *feature, LWLINE *lwline)
{
	size_t c;
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__LINESTRING;
	c = 2 + lwline->points->npoints * 2;
	feature->geometry = palloc(sizeof(*feature->geometry) * c);
	feature->n_geometry = encode_ptarray_initial(MVT_LINE,
		lwline->points, feature->geometry);
}

static void encode_mline(struct feature_builder *feature, LWMLINE *lwmline)
{
	uint32_t i;
	int32_t px = 0, py = 0;
	size_t c = 0, offset = 0;
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__LINESTRING;
	for (i = 0; i < lwmline->ngeoms; i++)
		c += 2 + lwmline->geoms[i]->points->npoints * 2;
	feature->geometry = palloc(sizeof(*feature->geometry) * c);
	for (i = 0; i < lwmline->ngeoms; i++)
		offset += encode_ptarray(MVT_LINE,
			lwmline->geoms[i]->points,
			feature->geometry + offset, &px, &py);
	feature->n_geometry = offset;
}

static void encode_poly(struct feature_builder *feature, LWPOLY *lwpoly)
{
	uint32_t i;
	int32_t px = 0, py = 0;
	size_t c = 0, offset = 0;
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__POLYGON;
	for (i = 0; i < lwpoly->nrings; i++)
		c += 3 + ((lwpoly->rings[i]->npoints - 1) * 2);
	feature->geometry = palloc(sizeof(*feature->geometry) * c);
	for (i = 0; i < lwpoly->nrings; i++)
		offset += encode_ptarray(MVT_RING,
			lwpoly->rings[i],
			feature->geometry + offset, &px, &py);
	feature->n_geometry = offset;
}

static void encode_mpoly(struct feature_builder *feature, LWMPOLY *lwmpoly)
{
	uint32_t i, j;
	int32_t px = 0, py = 0;
	size_t c = 0, offset = 0;
	LWPOLY *poly;
	feature->type = VECTOR_TILE__TILE__GEOM_TYPE__POLYGON;
	for (i = 0; i < lwmpoly->ngeoms; i++)
		for (j = 0; poly = lwmpoly->geoms[i], j < poly->nrings; j++)
			c += 3 + ((poly->rings[j]->npoints - 1) * 2);
	feature->geometry = palloc(sizeof(*feature->geometry) * c);
	for (i = 0; i < lwmpoly->ngeoms; i++)
		for (j = 0; poly = lwmpoly->geoms[i], j < poly->nrings; j++)
			offset += encode_ptarray(MVT_RING,
				poly->rings[j],	feature->geometry + offset,
				&px, &py);
	feature->n_geometry = offset;
}

static void encode_feature_geometry(struct feature_builder *feature, LWGEOM *lwgeom)
{
	int type = lwgeom->type;

	switch (type)
	{
	case POINTTYPE:
		return encode_point(feature, (LWPOINT*)lwgeom);
	case LINETYPE:
		return encode_line(feature, (LWLINE*)lwgeom);
	case POLYGONTYPE:
		return encode_poly(feature, (LWPOLY*)lwgeom);
	case MULTIPOINTTYPE:
		return encode_mpoint(feature, (LWMPOINT*)lwgeom);
	case MULTILINETYPE:
		return encode_mline(feature, (LWMLINE*)lwgeom);
	case MULTIPOLYGONTYPE:
		return encode_mpoly(feature, (LWMPOLY*)lwgeom);
	default: elog(ERROR, "encode_feature_geometry: '%s' geometry type not supported",
		lwtype_name(type));
	}
}

static TupleDesc get_tuple_desc(mvt_agg_context *ctx)
{
	Oid tupType = HeapTupleHeaderGetTypeId(ctx->row);
	int32 tupTypmod = HeapTupleHeaderGetTypMod(ctx->row);
	TupleDesc tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	return tupdesc;
}

static uint32_t get_key_index_with_size(mvt_agg_context *ctx, const char *name, size_t size)
{
	struct mvt_kv_key *kv;
	HASH_FIND(hh, ctx->keys_hash, name, size, kv);
	if (!kv)
		return UINT32_MAX;
	return kv->id;
}

static uint32_t add_key(mvt_agg_context *ctx, char *name)
{
	struct mvt_kv_key *kv;
	size_t size = strlen(name);
	kv = palloc(sizeof(*kv));
	kv->id = ctx->keys_hash_i++;
	kv->name = name;
	HASH_ADD_KEYPTR(hh, ctx->keys_hash, name, size, kv);
	return kv->id;
}

static void parse_column_keys(mvt_agg_context *ctx)
{
	uint32_t i, natts;
	bool geom_found = false;

	POSTGIS_DEBUG(2, "parse_column_keys called");

	ctx->column_cache.tupdesc = get_tuple_desc(ctx);
	natts = ctx->column_cache.tupdesc->natts;

	ctx->column_cache.column_keys_index = palloc(sizeof(uint32_t) * natts);
	ctx->column_cache.column_oid = palloc(sizeof(uint32_t) * natts);
	ctx->column_cache.values = palloc(sizeof(Datum) * natts);
	ctx->column_cache.nulls = palloc(sizeof(bool) * natts);

	for (i = 0; i < natts; i++)
	{
		Oid typoid = getBaseType(TupleDescAttr(ctx->column_cache.tupdesc, i)->atttypid);
		char *tkey = TupleDescAttr(ctx->column_cache.tupdesc, i)->attname.data;

		ctx->column_cache.column_oid[i] = typoid;

		if (typoid == JSONBOID)
		{
			ctx->column_cache.column_keys_index[i] = UINT32_MAX;
			continue;
		}

		if (ctx->geom_name == NULL)
		{
			if (!geom_found && typoid == postgis_oid(GEOMETRYOID))
			{
				ctx->geom_index = i;
				geom_found = true;
				continue;
			}
		}
		else
		{
			if (!geom_found && strcmp(tkey, ctx->geom_name) == 0)
			{
				ctx->geom_index = i;
				geom_found = true;
				continue;
			}
		}

		if (ctx->id_name &&
			(ctx->id_index == UINT32_MAX) &&
			(strcmp(tkey, ctx->id_name) == 0) &&
			(typoid == INT2OID || typoid == INT4OID || typoid == INT8OID))
		{
			ctx->id_index = i;
		}
		else
		{
			ctx->column_cache.column_keys_index[i] = add_key(ctx, pstrdup(tkey));
		}
	}

	if (!geom_found)
		elog(ERROR, "parse_column_keys: no geometry column found");

	if (ctx->id_name != NULL && ctx->id_index == UINT32_MAX)
		elog(ERROR, "mvt_agg_transfn: Could not find column '%s' of integer type", ctx->id_name);
}

static void encode_keys(mvt_agg_context *ctx)
{
	struct mvt_kv_key *kv;
	size_t n_keys = ctx->keys_hash_i;
	char **keys = palloc(n_keys * sizeof(*keys));
	for (kv = ctx->keys_hash; kv != NULL; kv=kv->hh.next)
		keys[kv->id] = kv->name;
	ctx->layer->n_keys = n_keys;
	ctx->layer->keys = keys;

	HASH_CLEAR(hh, ctx->keys_hash);
}

#define MVT_CREATE_VALUES(hash) \
	{ \
		struct mvt_kv_value *kv; \
		for (kv = hash; kv != NULL; kv = kv->hh.next) \
		{ \
			values[kv->id] = kv->value; \
		} \
	}

static void encode_values(mvt_agg_context *ctx)
{
	VectorTile__Tile__Value **values;
	POSTGIS_DEBUG(2, "encode_values called");

	values = palloc(ctx->values_hash_i * sizeof(*values));
	MVT_CREATE_VALUES(ctx->string_values_hash);
	MVT_CREATE_VALUES(ctx->float_values_hash);
	MVT_CREATE_VALUES(ctx->double_values_hash);
	MVT_CREATE_VALUES(ctx->uint_values_hash);
	MVT_CREATE_VALUES(ctx->sint_values_hash);
	MVT_CREATE_VALUES(ctx->bool_values_hash);

	POSTGIS_DEBUGF(3, "encode_values n_values: %d", ctx->values_hash_i);
	ctx->layer->n_values = ctx->values_hash_i;
	ctx->layer->values = values;

	/* Since the tupdesc is part of Postgresql cache, we need to ensure we release it when we
	 * are done with it */
	ReleaseTupleDesc(ctx->column_cache.tupdesc);
	memset(&ctx->column_cache, 0, sizeof(ctx->column_cache));

}

#define MVT_PARSE_VALUE(hash, newvalue, size, pfvaluefield, pftype) \
	{ \
		POSTGIS_DEBUG(2, "MVT_PARSE_VALUE called"); \
		{ \
			struct mvt_kv_value *kv; \
			unsigned hashv; \
			HASH_VALUE(&newvalue, size, hashv); \
			HASH_FIND_BYHASHVALUE(hh, ctx->hash, &newvalue, size, hashv, kv); \
			if (!kv) \
			{ \
				POSTGIS_DEBUG(4, "MVT_PARSE_VALUE value not found"); \
				kv = palloc(sizeof(*kv)); \
				POSTGIS_DEBUGF(4, "MVT_PARSE_VALUE new hash key: %d", ctx->values_hash_i); \
				kv->id = ctx->values_hash_i++; \
				vector_tile__tile__value__init(kv->value); \
				kv->value->pfvaluefield = newvalue; \
				kv->value->test_oneof_case = pftype; \
				HASH_ADD_KEYPTR_BYHASHVALUE(hh, ctx->hash, &kv->value->pfvaluefield, size, hashv, kv); \
			} \
			feature_add_property(feature, k, kv->id); \
		} \
	}

#define MVT_PARSE_INT_VALUE(value) \
	{ \
		if (value >= 0) \
		{ \
			uint64_t cvalue = value; \
			MVT_PARSE_VALUE(uint_values_hash, \
					cvalue, \
					sizeof(uint64_t), \
					uint_value, \
					VECTOR_TILE__TILE__VALUE__TEST_ONEOF_UINT_VALUE); \
		} \
		else \
		{ \
			int64_t cvalue = value; \
			MVT_PARSE_VALUE(sint_values_hash, \
					cvalue, \
					sizeof(int64_t), \
					sint_value, \
					VECTOR_TILE__TILE__VALUE__TEST_ONEOF_SINT_VALUE); \
		} \
	}

#define MVT_PARSE_DATUM(type, datumfunc, hash, size, pfvaluefield, pftype) \
	{ \
		type value = datumfunc(datum); \
		MVT_PARSE_VALUE(hash, value, size, pfvaluefield, pftype); \
	}

#define MVT_PARSE_INT_DATUM(type, datumfunc) \
{ \
	type value = datumfunc(datum); \
	MVT_PARSE_INT_VALUE(value); \
}

static bool
add_value_as_string_with_size(mvt_agg_context *ctx, struct feature_builder *feature, char *value, size_t size, uint32_t k)
{
	bool kept = false;
	struct mvt_kv_value *kv;
	unsigned hashv;
	HASH_VALUE(value, size, hashv);
	POSTGIS_DEBUG(2, "add_value_as_string called");
	HASH_FIND_BYHASHVALUE(hh, ctx->string_values_hash, value, size, hashv, kv);
	if (!kv)
	{
		POSTGIS_DEBUG(4, "add_value_as_string value not found");
		kv = palloc(sizeof(*kv));
		POSTGIS_DEBUGF(4, "add_value_as_string new hash key: %d",
			ctx->values_hash_i);
		kv->id = ctx->values_hash_i++;
		vector_tile__tile__value__init(kv->value);
		kv->value->string_value = value;
		kv->value->test_oneof_case = VECTOR_TILE__TILE__VALUE__TEST_ONEOF_STRING_VALUE;
		HASH_ADD_KEYPTR_BYHASHVALUE(hh, ctx->string_values_hash, kv->value->string_value, size, hashv, kv);
		kept = true;
	}

	feature_add_property(feature, k, kv->id);
	return kept;
}

/* Adds a string to the stored values. Gets ownership of the string */
static void
add_value_as_string(mvt_agg_context *ctx, struct feature_builder *feature, char *value, uint32_t k)
{
	bool kept = add_value_as_string_with_size(ctx, feature, value, strlen(value), k);
	if (!kept)
		pfree(value);
}

/* Adds a Datum to the stored values as a string. */
static inline void
parse_datum_as_string(mvt_agg_context *ctx, struct feature_builder *feature, Oid typoid, Datum datum, uint32_t k)
{
	Oid foutoid;
	bool typisvarlena;
	char *value;
	POSTGIS_DEBUG(2, "parse_value_as_string called");
	getTypeOutputInfo(typoid, &foutoid, &typisvarlena);
	value = OidOutputFunctionCall(foutoid, datum);
	POSTGIS_DEBUGF(4, "parse_value_as_string value: %s", value);
	add_value_as_string(ctx, feature, value, k);
}

static void parse_jsonb(mvt_agg_context *ctx, struct feature_builder *feature, Jsonb *jb)
{
	JsonbIterator *it;
	JsonbValue v;
	bool skipNested = false;
	JsonbIteratorToken r;
	uint32_t k;

	if (!JB_ROOT_IS_OBJECT(jb))
		return;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if (r == WJB_KEY && v.type != jbvNull)
		{

			k = get_key_index_with_size(ctx, v.val.string.val, v.val.string.len);
			if (k == UINT32_MAX)
			{
				char *key = palloc(v.val.string.len + 1);
				memcpy(key, v.val.string.val, v.val.string.len);
				key[v.val.string.len] = '\0';
				k = add_key(ctx, key);
			}

			r = JsonbIteratorNext(&it, &v, skipNested);

			if (v.type == jbvString)
			{
				char *value = palloc(v.val.string.len + 1);
				memcpy(value, v.val.string.val, v.val.string.len);
				value[v.val.string.len] = '\0';
				add_value_as_string(ctx, feature, value, k);
			}
			else if (v.type == jbvBool)
			{
				MVT_PARSE_VALUE(bool_values_hash,
						v.val.boolean,
						sizeof(protobuf_c_boolean),
						bool_value,
						VECTOR_TILE__TILE__VALUE__TEST_ONEOF_BOOL_VALUE);
			}
			else if (v.type == jbvNumeric)
			{
				char *str;
				double d;
				long l;
				str = DatumGetCString(DirectFunctionCall1(numeric_out,
					PointerGetDatum(v.val.numeric)));
				d = strtod(str, NULL);
				l = strtol(str, NULL, 10);

				if (fabs(d - (double)l) > FLT_EPSILON)
				{
					MVT_PARSE_VALUE(double_values_hash,
							d,
							sizeof(double),
							double_value,
							VECTOR_TILE__TILE__VALUE__TEST_ONEOF_DOUBLE_VALUE);
				}
				else
				{
					MVT_PARSE_INT_VALUE(l);
				}
			}
		}
	}
}

/**
 * Sets the feature id. Ignores Nulls and negative values
 */
static void set_feature_id(mvt_agg_context *ctx, struct feature_builder *feature, Datum datum, bool isNull)
{
	Oid typoid = ctx->column_cache.column_oid[ctx->id_index];
	int64_t value = INT64_MIN;

	if (isNull)
	{
		POSTGIS_DEBUG(3, "set_feature_id: Ignored null value");
		return;
	}

	switch (typoid)
	{
	case INT2OID:
		value = DatumGetInt16(datum);
		break;
	case INT4OID:
		value = DatumGetInt32(datum);
		break;
	case INT8OID:
		value = DatumGetInt64(datum);
		break;
	default:
		elog(ERROR, "set_feature_id: Feature id type does not match");
	}

	if (value < 0)
	{
		POSTGIS_DEBUG(3, "set_feature_id: Ignored negative value");
		return;
	}

	feature->has_id = true;
	feature->id = (uint64_t) value;
}

static void parse_values(mvt_agg_context *ctx, struct feature_builder *feature)
{
	uint32_t i;
	mvt_column_cache cc = ctx->column_cache;
	uint32_t natts = (uint32_t) cc.tupdesc->natts;

	HeapTupleData tuple;

	POSTGIS_DEBUG(2, "parse_values called");

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(ctx->row);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = ctx->row;

	/* We use heap_deform_tuple as it costs only O(N) vs O(N^2) of GetAttributeByNum */
	heap_deform_tuple(&tuple, cc.tupdesc, cc.values, cc.nulls);

	POSTGIS_DEBUGF(3, "parse_values natts: %d", natts);

	for (i = 0; i < natts; i++)
	{
		char *key;
		Oid typoid;
		uint32_t k;
		Datum datum = cc.values[i];

		if (i == ctx->geom_index)
			continue;

		if (i == ctx->id_index)
		{
			set_feature_id(ctx, feature, datum, cc.nulls[i]);
			continue;
		}

		if (cc.nulls[i])
		{
			POSTGIS_DEBUG(3, "parse_values isnull detected");
			continue;
		}

		key = TupleDescAttr(cc.tupdesc, i)->attname.data;
		k = cc.column_keys_index[i];
		typoid = cc.column_oid[i];

		if (k == UINT32_MAX && typoid != JSONBOID)
			elog(ERROR, "parse_values: unexpectedly could not find parsed key name '%s'", key);
		if (typoid == JSONBOID)
		{
			parse_jsonb(ctx, feature, DatumGetJsonbP(datum));
			continue;
		}

		switch (typoid)
		{
		case BOOLOID:
			MVT_PARSE_DATUM(protobuf_c_boolean,
					DatumGetBool,
					bool_values_hash,
					sizeof(protobuf_c_boolean),
					bool_value,
					VECTOR_TILE__TILE__VALUE__TEST_ONEOF_BOOL_VALUE);
			break;
		case INT2OID:
			MVT_PARSE_INT_DATUM(int16_t, DatumGetInt16);
			break;
		case INT4OID:
			MVT_PARSE_INT_DATUM(int32_t, DatumGetInt32);
			break;
		case INT8OID:
			MVT_PARSE_INT_DATUM(int64_t, DatumGetInt64);
			break;
		case FLOAT4OID:
			MVT_PARSE_DATUM(float,
					DatumGetFloat4,
					float_values_hash,
					sizeof(float),
					float_value,
					VECTOR_TILE__TILE__VALUE__TEST_ONEOF_FLOAT_VALUE);
			break;
		case FLOAT8OID:
			MVT_PARSE_DATUM(double,
					DatumGetFloat8,
					double_values_hash,
					sizeof(double),
					double_value,
					VECTOR_TILE__TILE__VALUE__TEST_ONEOF_DOUBLE_VALUE);
			break;
		case TEXTOID:
			add_value_as_string(ctx, feature, text_to_cstring(DatumGetTextP(datum)), k);
			break;
		case CSTRINGOID:
			add_value_as_string(ctx, feature, DatumGetCString(datum), k);
			break;
		default:
			parse_datum_as_string(ctx, feature, typoid, datum, k);
			break;
		}
	}
}

/* For a given geometry, look for the highest dimensional basic type, that is,
 * point, line or polygon */
static uint8_t
lwgeom_get_basic_type(LWGEOM *geom)
{
	switch(geom->type)
	{
	case POINTTYPE:
	case LINETYPE:
	case POLYGONTYPE:
		return geom->type;
	case TRIANGLETYPE:
		return POLYGONTYPE;
	case MULTIPOINTTYPE:
	case MULTILINETYPE:
	case MULTIPOLYGONTYPE:
		return geom->type - 3; /* Based on LWTYPE positions */
	case COLLECTIONTYPE:
	case TINTYPE:
	{
		uint32_t i;
		uint8_t type = 0;
		LWCOLLECTION *g = (LWCOLLECTION*)geom;
		for (i = 0; i < g->ngeoms; i++)
		{
			LWGEOM *sg = g->geoms[i];
			type = Max(type, lwgeom_get_basic_type(sg));
		}
		return type;
	}
	default:
		elog(ERROR, "%s: Invalid type (%d)", __func__, geom->type);
	}
}


/**
 * In place process a collection to find a concrete geometry
 * object and expose that as the actual object. Will some
 * geom be lost? Sure, but your MVT renderer couldn't
 * draw it anyways.
 */
static inline LWGEOM *
lwgeom_to_basic_type(LWGEOM *geom, uint8_t original_type)
{
	LWGEOM *geom_out = geom;
	if (lwgeom_get_type(geom) == COLLECTIONTYPE)
	{
		LWCOLLECTION *g = (LWCOLLECTION*)geom;
		geom_out = (LWGEOM *)lwcollection_extract(g, original_type);
	}

	/* If a collection only contains 1 geometry return than instead */
	if (lwgeom_is_collection(geom_out))
	{
		LWCOLLECTION *g = (LWCOLLECTION *)geom_out;
		if (g->ngeoms == 1)
		{
			geom_out = g->geoms[0];
		}
	}

	geom_out->srid = geom->srid;
	return geom_out;
}

/* Clips a geometry using lwgeom_clip_by_rect. Might return NULL */
static LWGEOM *
mvt_unsafe_clip_by_box(LWGEOM *lwg_in, GBOX *clip_box)
{
	LWGEOM *geom_clipped;
	GBOX geom_box;

	gbox_init(&geom_box);
	FLAGS_SET_GEODETIC(geom_box.flags, 0);
	lwgeom_calculate_gbox(lwg_in, &geom_box);

	if (!gbox_overlaps_2d(&geom_box, clip_box))
	{
		POSTGIS_DEBUG(3, "mvt_geom: geometry outside clip box");
		return NULL;
	}

	if (gbox_contains_2d(clip_box, &geom_box))
	{
		POSTGIS_DEBUG(3, "mvt_geom: geometry contained fully inside the box");
		return lwg_in;
	}

	geom_clipped = lwgeom_clip_by_rect(lwg_in, clip_box->xmin, clip_box->ymin, clip_box->xmax, clip_box->ymax);
	if (!geom_clipped || lwgeom_is_empty(geom_clipped))
		return NULL;
	return geom_clipped;
}

/* Clips a geometry for MVT using GEOS.
 * Does NOT work for polygons
 * Might return NULL
 */
static LWGEOM *
mvt_clip_and_validate_geos(LWGEOM *lwgeom, uint8_t basic_type, uint32_t extent, uint32_t buffer, bool clip_geom)
{
	LWGEOM *ng = lwgeom;
	assert(lwgeom->type != POLYGONTYPE);
	assert(lwgeom->type != MULTIPOLYGONTYPE);

	if (clip_geom)
	{
		gridspec grid = {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0};
		GBOX bgbox;
		bgbox.xmax = bgbox.ymax = (double)extent + (double)buffer;
		bgbox.xmin = bgbox.ymin = -(double)buffer;
		bgbox.flags = 0;

		ng = mvt_unsafe_clip_by_box(ng, &bgbox);

		/* Make sure there is no pending float values (clipping can do that) */
		lwgeom_grid_in_place(ng, &grid);
	}

	return ng;
}

static LWGEOM *
mvt_clip_and_validate(LWGEOM *lwgeom, uint8_t basic_type, uint32_t extent, uint32_t buffer, bool clip_geom)
{
	GBOX clip_box = {0};
	LWGEOM *clipped_lwgeom;

	/* Wagyu only supports polygons. Default to geos for other types */
	lwgeom = lwgeom_to_basic_type(lwgeom, POLYGONTYPE);
	if (lwgeom->type != POLYGONTYPE && lwgeom->type != MULTIPOLYGONTYPE)
	{
		return mvt_clip_and_validate_geos(lwgeom, basic_type, extent, buffer, clip_geom);
	}

	if (!clip_geom)
	{
		/* With clipping disabled, we request a clip with the geometry bbox to force validation */
		lwgeom_calculate_gbox(lwgeom, &clip_box);
	}
	else
	{
		clip_box.xmax = clip_box.ymax = (double)extent + (double)buffer;
		clip_box.xmin = clip_box.ymin = -(double)buffer;
	}

	clipped_lwgeom = lwgeom_wagyu_clip_by_box(lwgeom, &clip_box);

	return clipped_lwgeom;
}

/**
 * Transform a geometry into vector tile coordinate space.
 *
 * Makes best effort to keep validity. Might collapse geometry into lower
 * dimension.
 *
 * NOTE: modifies in place if possible (not currently possible for polygons)
 */
LWGEOM *mvt_geom(LWGEOM *lwgeom, const GBOX *gbox, uint32_t extent, uint32_t buffer,
	bool clip_geom)
{
	AFFINE affine = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	gridspec grid = {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0};
	double width = gbox->xmax - gbox->xmin;
	double height = gbox->ymax - gbox->ymin;
	double fx, fy;
	const uint8_t basic_type = lwgeom_get_basic_type(lwgeom);
	int preserve_collapsed = LW_FALSE;
	POSTGIS_DEBUG(2, "mvt_geom called");

	/* Simplify it as soon as possible */
	lwgeom = lwgeom_to_basic_type(lwgeom, basic_type);

	/* Short circuit out on EMPTY */
	if (lwgeom_is_empty(lwgeom))
		return NULL;

	fx = extent / width;
	fy = -(extent / height);

	/* If geometry has disappeared, you're done */
	if (lwgeom_is_empty(lwgeom))
		return NULL;

	/* transform to tile coordinate space */
	affine.afac = fx;
	affine.efac = fy;
	affine.ifac = 1;
	affine.xoff = -gbox->xmin * fx;
	affine.yoff = -gbox->ymax * fy;
	lwgeom_affine(lwgeom, &affine);

	/* Snap to integer precision, removing duplicate points */
	lwgeom_grid_in_place(lwgeom, &grid);

	/* Remove points on straight lines */
	lwgeom_simplify_in_place(lwgeom, 0, preserve_collapsed);

	/* Remove duplicates in multipoints */
	if (lwgeom->type == MULTIPOINTTYPE)
		lwgeom_remove_repeated_points_in_place(lwgeom, 0);

	if (!lwgeom || lwgeom_is_empty(lwgeom))
		return NULL;

	lwgeom = mvt_clip_and_validate(lwgeom, basic_type, extent, buffer, clip_geom);
	if (!lwgeom || lwgeom_is_empty(lwgeom))
		return NULL;

	return lwgeom;
}

/**
 * Initialize aggregation context.
 */
void mvt_agg_init_context(mvt_agg_context *ctx)
{
	VectorTile__Tile__Layer *layer;

	POSTGIS_DEBUG(2, "mvt_agg_init_context called");

	if (ctx->extent == 0)
		elog(ERROR, "mvt_agg_init_context: extent cannot be 0");

	ctx->tile = NULL;
	ctx->features_capacity = FEATURES_CAPACITY_INITIAL;
	ctx->keys_hash = NULL;
	ctx->string_values_hash = NULL;
	ctx->float_values_hash = NULL;
	ctx->double_values_hash = NULL;
	ctx->uint_values_hash = NULL;
	ctx->sint_values_hash = NULL;
	ctx->bool_values_hash = NULL;
	ctx->values_hash_i = 0;
	ctx->keys_hash_i = 0;
	ctx->id_index = UINT32_MAX;
	ctx->geom_index = UINT32_MAX;

	memset(&ctx->column_cache, 0, sizeof(ctx->column_cache));

	layer = palloc(sizeof(*layer));
	vector_tile__tile__layer__init(layer);
	layer->version = 2;
	layer->name = ctx->name;
	layer->extent = ctx->extent;
	layer->features = palloc(ctx->features_capacity * sizeof(*layer->features));

	ctx->layer = layer;
}

/**
 * Aggregation step. Parse a row, turn it into a feature, and add it to the layer.
 *
 * Expands features array if needed by a factor of 2.
 * Allocates a new feature, increment feature counter and
 * encode geometry and properties into it.
 */
void mvt_agg_transfn(mvt_agg_context *ctx)
{
	bool isnull = false;
	Datum datum;
	GSERIALIZED *gs;
	LWGEOM *lwgeom;
	struct feature_builder feature_builder;
	VectorTile__Tile__Layer *layer = ctx->layer;
	POSTGIS_DEBUG(2, "mvt_agg_transfn called");

	/* geom_index is the cached index of the geometry. if missing, it needs to be initialized */
	if (ctx->geom_index == UINT32_MAX)
		parse_column_keys(ctx);

	/* Get the geometry column */
	datum = GetAttributeByNum(ctx->row, ctx->geom_index + 1, &isnull);
	if (isnull) /* Skip rows that have null geometry */
		return;

	/* Allocate a new feature object */
	feature_init(&feature_builder);

	/* Deserialize the geometry */
	gs = (GSERIALIZED *) PG_DETOAST_DATUM(datum);
	lwgeom = lwgeom_from_gserialized(gs);

	/* Set the geometry of the feature */
	encode_feature_geometry(&feature_builder, lwgeom);
	lwgeom_free(lwgeom);
	// TODO: free detoasted datum?

	/* Parse properties */
	parse_values(ctx, &feature_builder);

	/* Grow the features array of the layer */
	POSTGIS_DEBUGF(3, "mvt_agg_transfn encoded feature count: %zd", layer->n_features);
	if (layer->n_features >= ctx->features_capacity)
	{
		size_t new_capacity = ctx->features_capacity * 2;
		layer->features = repalloc(layer->features, new_capacity *
			sizeof(*layer->features));
		ctx->features_capacity = new_capacity;
		POSTGIS_DEBUGF(3, "mvt_agg_transfn new_capacity: %zd", new_capacity);
	}

	/* Build and add the feature to the layer */
	layer->features[layer->n_features++] = feature_build(&feature_builder);
}

static VectorTile__Tile * mvt_ctx_to_tile(mvt_agg_context *ctx)
{
	int n_layers = 1;
	VectorTile__Tile *tile;
	encode_keys(ctx);
	encode_values(ctx);

	tile = palloc(sizeof(VectorTile__Tile));
	vector_tile__tile__init(tile);
	tile->layers = palloc(sizeof(VectorTile__Tile__Layer*) * n_layers);
	tile->layers[0] = ctx->layer;
	tile->n_layers = n_layers;
	return tile;
}

static bytea *mvt_ctx_to_bytea(mvt_agg_context *ctx)
{
	/* Fill out the tile slot, if it's not already filled. */
	/* We should only have a filled slot when all the work of building */
	/* out the data is complete, so after a serialize/deserialize cycle */
	/* or after a context combine */
	size_t len;
	bytea *ba;

	if (!ctx->tile)
	{
		ctx->tile = mvt_ctx_to_tile(ctx);
	}

	/* Zero features => empty bytea output */
	if (ctx && ctx->layer && ctx->layer->n_features == 0)
	{
		bytea* ba_empty = palloc(VARHDRSZ);
		SET_VARSIZE(ba_empty, VARHDRSZ);
		return ba_empty;
	}

	/* Serialize the Tile */
	len = VARHDRSZ + vector_tile__tile__get_packed_size(ctx->tile);
	ba = palloc(len);
	vector_tile__tile__pack(ctx->tile, (uint8_t*)VARDATA(ba));
	SET_VARSIZE(ba, len);
	return ba;
}


bytea * mvt_ctx_serialize(mvt_agg_context *ctx)
{
	return mvt_ctx_to_bytea(ctx);
}

static void * mvt_allocator(__attribute__((__unused__)) void *data, size_t size)
{
	return palloc(size);
}

static void mvt_deallocator(__attribute__((__unused__)) void *data, void *ptr)
{
	return pfree(ptr);
}

mvt_agg_context * mvt_ctx_deserialize(const bytea *ba)
{
	ProtobufCAllocator allocator =
	{
		mvt_allocator,
		mvt_deallocator,
		NULL
	};

	size_t len = VARSIZE_ANY_EXHDR(ba);
	VectorTile__Tile *tile = vector_tile__tile__unpack(&allocator, len, (uint8_t*)VARDATA(ba));
	mvt_agg_context *ctx = palloc(sizeof(mvt_agg_context));
	memset(ctx, 0, sizeof(mvt_agg_context));
	ctx->tile = tile;
	return ctx;
}

/**
 * Combine 2 layers. This is going to push everything from layer2 into layer1
 * We can do this because both sources and the result live in the same aggregation context
 * so we are good as long as we don't free anything from the sources
 *
 * TODO: Apply hash to remove duplicates (https://trac.osgeo.org/postgis/ticket/4310)
 */
static VectorTile__Tile__Layer *
vectortile_layer_combine(VectorTile__Tile__Layer *layer, VectorTile__Tile__Layer *layer2)
{
	const uint32_t key_offset = layer->n_keys;
	const uint32_t value_offset = layer->n_values;
	const uint32_t feature_offset = layer->n_features;

	if (!layer->n_keys)
	{
		layer->keys = layer2->keys;
		layer->n_keys = layer2->n_keys;
	}
	else if (layer2->n_keys)
	{
		layer->keys = repalloc(layer->keys, sizeof(char *) * (layer->n_keys + layer2->n_keys));
		memcpy(&layer->keys[key_offset], layer2->keys, sizeof(char *) * layer2->n_keys);
		layer->n_keys += layer2->n_keys;
	}

	if (!layer->n_values)
	{
		layer->values = layer2->values;
		layer->n_values = layer2->n_values;
	}
	else if (layer2->n_values)
	{
		layer->values =
		    repalloc(layer->values, sizeof(VectorTile__Tile__Value *) * (layer->n_values + layer2->n_values));
		memcpy(
		    &layer->values[value_offset], layer2->values, sizeof(VectorTile__Tile__Value *) * layer2->n_values);
		layer->n_values += layer2->n_values;
	}

	if (!layer->n_features)
	{
		layer->features = layer2->features;
		layer->n_features = layer2->n_features;
	}
	else if (layer2->n_features)
	{
		layer->features = repalloc(
		    layer->features, sizeof(VectorTile__Tile__Feature *) * (layer->n_features + layer2->n_features));
		memcpy(&layer->features[feature_offset], layer2->features, sizeof(char *) * layer2->n_features);
		layer->n_features += layer2->n_features;
		/* We need to adapt the indexes of the copied features */
		for (uint32_t i = feature_offset; i < layer->n_features; i++)
		{
			for (uint32_t t = 0; t < layer->features[i]->n_tags; t += 2)
			{
				layer->features[i]->tags[t] += key_offset;
				layer->features[i]->tags[t + 1] += value_offset;
			}
		}
	}

	return layer;
}


static VectorTile__Tile *
vectortile_tile_combine(VectorTile__Tile *tile1, VectorTile__Tile *tile2)
{
	uint32_t i, j;
	VectorTile__Tile *tile;

	/* Hopelessly messing up memory ownership here */
	if (tile1->n_layers == 0 && tile2->n_layers == 0)
		return tile1;
	else if (tile1->n_layers == 0)
		return tile2;
	else if (tile2->n_layers == 0)
		return tile1;

	tile = palloc(sizeof(VectorTile__Tile));
	vector_tile__tile__init(tile);
	tile->layers = palloc(sizeof(void*));
	tile->n_layers = 0;

	/* Merge all matching layers in the files (basically always only one) */
	for (i = 0; i < tile1->n_layers; i++)
	{
		for (j = 0; j < tile2->n_layers; j++)
		{
			VectorTile__Tile__Layer *l1 = tile1->layers[i];
			VectorTile__Tile__Layer *l2 = tile2->layers[j];
			if (strcmp(l1->name, l2->name)==0)
			{
				VectorTile__Tile__Layer *layer = vectortile_layer_combine(l1, l2);
				if (!layer)
					continue;
				tile->layers[tile->n_layers++] = layer;
				/* Add a spare slot at the end of the array */
				tile->layers = repalloc(tile->layers, (tile->n_layers+1) * sizeof(void*));
			}
		}
	}
	return tile;
}

mvt_agg_context * mvt_ctx_combine(mvt_agg_context *ctx1, mvt_agg_context *ctx2)
{
	if (ctx1 || ctx2)
	{
		if (ctx1 && ! ctx2) return ctx1;
		if (ctx2 && ! ctx1) return ctx2;
		if (ctx1 && ctx2 && ctx1->tile && ctx2->tile)
		{
			mvt_agg_context *ctxnew = palloc(sizeof(mvt_agg_context));
			memset(ctxnew, 0, sizeof(mvt_agg_context));
			ctxnew->tile = vectortile_tile_combine(ctx1->tile, ctx2->tile);
			return ctxnew;
		}
		else
		{
			elog(DEBUG2, "ctx1->tile = %p", ctx1->tile);
			elog(DEBUG2, "ctx2->tile = %p", ctx2->tile);
			elog(ERROR, "%s: unable to combine contexts where tile attribute is null", __func__);
			return NULL;
		}
	}
	else
	{
		return NULL;
	}
}

/**
 * Finalize aggregation.
 *
 * Encode keys and values and put the aggregated Layer message into
 * a Tile message and returns it packed as a bytea.
 */
bytea *mvt_agg_finalfn(mvt_agg_context *ctx)
{
	return mvt_ctx_to_bytea(ctx);
}


#endif
