#include <rte_hash.h>
#include <time.h>

#define GRT_RT_S_IDLE		0
#define GRT_RT_S_SNAPSHOTTING	1

typedef struct {
	struct rte_hash* hashmap;
	int state;
	uint32_t current_generation;
	uint16_t key_len;	
	uint16_t value_len;	//Used for copying and serializing keys and values.
	
} grt_redirect_table;



grt_redirect_table* grt_redirect_table_create(const struct rte_hash_parameters*, uint16_t, uint16_t);

void grt_redirect_table_free(grt_redirect_table*);

int grt_redirect_table_put(grt_redirect_table*, void*, void*, int64_t);

int grt_redirect_table_put_notifystupd(grt_redirect_table* tbl, void* key, void* value, int64_t timeout, uint16_t stupd_type);

int grt_redirect_table_put_fromstupd(grt_redirect_table* tbl, void* stupd);

int grt_redirect_table_remove(grt_redirect_table* tbl, void* key);

int grt_redirect_table_get(grt_redirect_table*, void*, void**, int);

int grt_redirect_iterate_snapshot(grt_redirect_table*, const void**, void**, int64_t*, uint32_t*, int);

void grt_redirect_set_snap_state(grt_redirect_table*, int);

int grt_redirect_dump_debug(grt_redirect_table*);

int grt_redirect_serialize_snapshot(grt_redirect_table* tbl, void** offset, void* maxOffset);

int grt_redirect_deserialize_snapshot(grt_redirect_table* tbl, void** offset);

