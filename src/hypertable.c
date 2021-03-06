#include <postgres.h>
#include <access/htup_details.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/memutils.h>
#include <utils/builtins.h>
#include <utils/acl.h>
#include <nodes/memnodes.h>
#include <catalog/namespace.h>
#include <commands/tablespace.h>
#include <commands/dbcommands.h>
#include <miscadmin.h>

#include "hypertable.h"
#include "dimension.h"
#include "chunk.h"
#include "compat.h"
#include "subspace_store.h"
#include "hypertable_cache.h"
#include "trigger.h"
#include "scanner.h"
#include "catalog.h"
#include "dimension_slice.h"
#include "dimension_vector.h"
#include "hypercube.h"

static Oid
rel_get_owner(Oid relid)
{
	HeapTuple	tuple;
	Oid			ownerid;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", relid)));

	ownerid = ((Form_pg_class) GETSTRUCT(tuple))->relowner;

	ReleaseSysCache(tuple);

	return ownerid;
}

bool
hypertable_has_privs_of(Oid hypertable_oid, Oid userid)
{
	return has_privs_of_role(userid, rel_get_owner(hypertable_oid));
}

Oid
hypertable_permissions_check(Oid hypertable_oid, Oid userid)
{
	Oid			ownerid = rel_get_owner(hypertable_oid);

	if (!has_privs_of_role(userid, ownerid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("User \"%s\" lacks permissions on table \"%s\"",
						GetUserNameFromId(userid, true),
						get_rel_name(hypertable_oid))));

	return ownerid;
}


Hypertable *
hypertable_from_tuple(HeapTuple tuple)
{
	Hypertable *h;
	Oid			namespace_oid;

	h = palloc0(sizeof(Hypertable));
	memcpy(&h->fd, GETSTRUCT(tuple), sizeof(FormData_hypertable));
	namespace_oid = get_namespace_oid(NameStr(h->fd.schema_name), false);
	h->main_table_relid = get_relname_relid(NameStr(h->fd.table_name), namespace_oid);
	h->space = dimension_scan(h->fd.id, h->main_table_relid, h->fd.num_dimensions);
	h->chunk_cache = subspace_store_init(h->space->num_dimensions, CurrentMemoryContext);

	return h;
}

static bool
hypertable_tuple_get_relid(TupleInfo *ti, void *data)
{
	FormData_hypertable *form = (FormData_hypertable *) GETSTRUCT(ti->tuple);
	Oid		   *relid = data;
	Oid			schema_oid = get_namespace_oid(NameStr(form->schema_name), true);

	if (OidIsValid(schema_oid))
		*relid = get_relname_relid(NameStr(form->table_name), schema_oid);

	return false;
}

Oid
hypertable_id_to_relid(int32 hypertable_id)
{
	Catalog    *catalog = catalog_get();
	Oid			relid = InvalidOid;
	ScanKeyData scankey[1];
	ScannerCtx	scanctx = {
		.table = catalog->tables[HYPERTABLE].id,
		.index = catalog->tables[HYPERTABLE].index_ids[HYPERTABLE_ID_INDEX],
		.scantype = ScannerTypeIndex,
		.nkeys = 1,
		.scankey = scankey,
		.tuple_found = hypertable_tuple_get_relid,
		.data = &relid,
		.lockmode = AccessShareLock,
		.scandirection = ForwardScanDirection,
	};

	/* Perform an index scan on the hypertable pkey. */
	ScanKeyInit(&scankey[0], Anum_hypertable_pkey_idx_id,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(hypertable_id));

	scanner_scan(&scanctx);

	return relid;
}

typedef struct ChunkCacheEntry
{
	MemoryContext mcxt;
	Chunk	   *chunk;
} ChunkCacheEntry;

static void
chunk_cache_entry_free(void *cce)
{
	MemoryContextDelete(((ChunkCacheEntry *) cce)->mcxt);
}

static int
hypertable_scan_limit_internal(ScanKeyData *scankey,
							   int num_scankeys,
							   int indexid,
							   tuple_found_func on_tuple_found,
							   void *scandata,
							   int limit,
							   LOCKMODE lock)
{
	Catalog    *catalog = catalog_get();
	ScannerCtx	scanctx = {
		.table = catalog->tables[HYPERTABLE].id,
		.index = catalog->tables[HYPERTABLE].index_ids[indexid],
		.scantype = ScannerTypeIndex,
		.nkeys = num_scankeys,
		.scankey = scankey,
		.data = scandata,
		.limit = limit,
		.tuple_found = on_tuple_found,
		.lockmode = lock,
		.scandirection = ForwardScanDirection,
	};

	return scanner_scan(&scanctx);
}

static bool
hypertable_tuple_update(TupleInfo *ti, void *data)
{
	HeapTuple	tuple = heap_copytuple(ti->tuple);
	FormData_hypertable *form = (FormData_hypertable *) GETSTRUCT(tuple);
	FormData_hypertable *update = data;
	CatalogSecurityContext sec_ctx;

	namecpy(&form->schema_name, &update->schema_name);
	namecpy(&form->table_name, &update->table_name);
	catalog_become_owner(catalog_get(), &sec_ctx);
	catalog_update(ti->scanrel, tuple);
	catalog_restore_user(&sec_ctx);

	heap_freetuple(tuple);

	return false;
}

static int
hypertable_update_form(FormData_hypertable *update)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0], Anum_hypertable_pkey_idx_id,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(update->id));

	return hypertable_scan_limit_internal(scankey,
										  1,
										  HYPERTABLE_ID_INDEX,
										  hypertable_tuple_update,
										  update,
										  1,
										  RowExclusiveLock);
}

int
hypertable_set_name(Hypertable *ht, const char *newname)
{
	namestrcpy(&ht->fd.table_name, newname);

	return hypertable_update_form(&ht->fd);
}

int
hypertable_set_schema(Hypertable *ht, const char *newname)
{
	namestrcpy(&ht->fd.schema_name, newname);

	return hypertable_update_form(&ht->fd);
}

Chunk *
hypertable_get_chunk(Hypertable *h, Point *point)
{
	ChunkCacheEntry *cce = subspace_store_get(h->chunk_cache, point);

	if (NULL == cce)
	{
		MemoryContext old_mcxt,
					chunk_mcxt;
		Chunk	   *chunk;

		/*
		 * chunk_find() must execute on a per-tuple memory context since it
		 * allocates a lot of transient data. We don't want this allocated on
		 * the cache's memory context.
		 */
		chunk = chunk_find(h->space, point);

		if (NULL == chunk)
			chunk = chunk_create(h, point,
								 NameStr(h->fd.associated_schema_name),
								 NameStr(h->fd.associated_table_prefix));

		Assert(chunk != NULL);

		chunk_mcxt = AllocSetContextCreate(subspace_store_mcxt(h->chunk_cache),
										   "chunk cache memory context",
										   ALLOCSET_SMALL_SIZES);

		old_mcxt = MemoryContextSwitchTo(chunk_mcxt);

		cce = palloc(sizeof(ChunkCacheEntry));
		cce->mcxt = chunk_mcxt;

		/* Make a copy which lives in the chunk cache's memory context */
		chunk = cce->chunk = chunk_copy(chunk);

		subspace_store_add(h->chunk_cache, chunk->cube, cce, chunk_cache_entry_free);
		MemoryContextSwitchTo(old_mcxt);
	}

	Assert(NULL != cce);
	Assert(NULL != cce->chunk);
	Assert(MemoryContextContains(cce->mcxt, cce));
	Assert(MemoryContextContains(cce->mcxt, cce->chunk));

	return cce->chunk;
}

bool
hypertable_has_tablespace(Hypertable *ht, Oid tspc_oid)
{
	Tablespaces *tspcs = tablespace_scan(ht->fd.id);

	return tablespaces_contain(tspcs, tspc_oid);
}

/*
 * Select a tablespace to use for a given chunk.
 *
 * Selection happens based on the first closed (space) dimension, if available,
 * otherwise the first closed (time) one.
 *
 * We try to do "sticky" selection to consistently pick the same tablespace for
 * chunks in the same closed (space) dimension. This ensures chunks in the same
 * "space" partition will live on the same disk.
 */
char *
hypertable_select_tablespace(Hypertable *ht, Chunk *chunk)
{
	Dimension  *dim;
	DimensionVec *vec;
	DimensionSlice *slice;
	Tablespaces *tspcs = tablespace_scan(ht->fd.id);
	int			i = 0;

	if (NULL == tspcs || tspcs->num_tablespaces == 0)
		return NULL;

	dim = hyperspace_get_closed_dimension(ht->space, 0);

	if (NULL == dim)
		dim = hyperspace_get_open_dimension(ht->space, 0);

	Assert(NULL != dim && (IS_OPEN_DIMENSION(dim) || dim->fd.num_slices > 0));

	vec = dimension_get_slices(dim);

	Assert(NULL != vec && (IS_OPEN_DIMENSION(dim) || vec->num_slices > 0));

	slice = hypercube_get_slice_by_dimension_id(chunk->cube, dim->fd.id);

	Assert(NULL != slice);

	/*
	 * Find the index (ordinal) of the chunk's slice in the dimension we
	 * picked
	 */
	i = dimension_vec_find_slice_index(vec, slice->fd.id);

	Assert(i >= 0);

	/* Use the index of the slice to find the tablespace */
	return NameStr(tspcs->tablespaces[i % tspcs->num_tablespaces].fd.tablespace_name);
}

static inline Oid
hypertable_relid_lookup(Oid relid)
{
	Cache	   *hcache = hypertable_cache_pin();
	Hypertable *ht = hypertable_cache_get_entry(hcache, relid);
	Oid			result = (ht == NULL) ? InvalidOid : ht->main_table_relid;

	cache_release(hcache);

	return result;
}

/*
 * Returns a hypertable's relation ID (OID) iff the given RangeVar corresponds to
 * a hypertable, otherwise InvalidOid.
*/
Oid
hypertable_relid(RangeVar *rv)
{
	return hypertable_relid_lookup(RangeVarGetRelid(rv, NoLock, true));
}

bool
is_hypertable(Oid relid)
{
	if (!OidIsValid(relid))
		return false;
	return hypertable_relid_lookup(relid) != InvalidOid;
}

TS_FUNCTION_INFO_V1(hypertable_validate_triggers);

Datum
hypertable_validate_triggers(PG_FUNCTION_ARGS)
{
	if (relation_has_transition_table_trigger(PG_GETARG_OID(0)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("Hypertables do not support transition tables in triggers.")));

	PG_RETURN_VOID();
}

TS_FUNCTION_INFO_V1(hypertable_check_associated_schema_permissions);

/*
 * Check that the current user can create chunks in a hypertable's associated
 * schema.
 *
 * This function is typically called from create_hypertable() to verify that the
 * table owner has CREATE permissions for the schema (if it already exists) or
 * the database (if the schema does not exist and needs to be created).
 */
Datum
hypertable_check_associated_schema_permissions(PG_FUNCTION_ARGS)
{
	Name		schema_name;
	Oid			schema_oid;
	Oid			user_oid;

	if (PG_NARGS() != 2)
		elog(ERROR, "Invalid number of arguments");

	/*
	 * If the schema name is NULL, it implies the internal catalog schema and
	 * anyone should be able to create chunks there.
	 */
	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();

	schema_name = PG_GETARG_NAME(0);

	/* Anyone can create chunks in the internal schema */
	if (namestrcmp(schema_name, INTERNAL_SCHEMA_NAME) == 0)
		PG_RETURN_VOID();

	if (PG_ARGISNULL(1))
		user_oid = GetUserId();
	else
		user_oid = PG_GETARG_OID(1);

	schema_oid = get_namespace_oid(NameStr(*schema_name), true);

	if (!OidIsValid(schema_oid))
	{
		/*
		 * Schema does not exist, so we must check that the user has
		 * privileges to create the schema in the current database
		 */
		if (pg_database_aclcheck(MyDatabaseId, user_oid, ACL_CREATE) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("User %s lacks permissions to create schema \"%s\" in database \"%s\"",
							GetUserNameFromId(user_oid, false),
							NameStr(*schema_name),
							get_database_name(MyDatabaseId))));
	}
	else if (pg_namespace_aclcheck(schema_oid, user_oid, ACL_CREATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		errmsg("User %s lacks permissions to create chunks in schema \"%s\"",
			   GetUserNameFromId(user_oid, false),
			   NameStr(*schema_name))));

	PG_RETURN_VOID();
}
