//Wrapper for rte_hash to allow for redirect-on-write
//All accesses to the map must go over this class, otherwise inconsistency may occur.

#include <stdio.h>
#include <stdlib.h>

#include <rte_config.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_branch_prediction.h>

#include "grt_redirect_table.h"
#include "grt_main.h"


//#define grt_DEBUG_REDIRTABLE

//TODO: Delete bucket???
//TODO: If entries must be changed without adding or removing, we need some type of "entry_change, 
//that copies an entry in case we do a snapshot"...


typedef struct  {

	void* a;
	uint32_t generation_a;
	void* b;
	uint32_t generation_b;

	int64_t timeout_a;	//if <0 Never time out. Otherwise time() at creation plus timeout.
	int64_t timeout_b;

} grt_redirect_field;






grt_redirect_table* grt_redirect_table_create(const struct rte_hash_parameters* rte_hash_params, uint16_t key_len, uint16_t value_len) {

	grt_redirect_table* new_tbl = rte_malloc("grt_redirect_table", sizeof(grt_redirect_table), 0);

	new_tbl->hashmap = rte_hash_create(rte_hash_params);
	new_tbl->state = GRT_RT_S_IDLE;
	new_tbl->current_generation = 1;
	new_tbl->key_len = key_len;
	new_tbl->value_len = value_len;

	return new_tbl;

}

void grt_redirect_table_free(grt_redirect_table* old_tbl) {
	int32_t iter = 0;
	void* next_key;
	grt_redirect_field* next_field;
	int32_t return_val;
	while (return_val = rte_hash_iterate(old_tbl->hashmap, (const void**)&next_key, (void**)&next_field, &iter) >= 0) {
		//printf("Found: (%u, %u) -> (%u, %u), return_val=%i, iter=%i.\n", next_key->a, 
		//	next_key->b, next_value->c, next_value->d, return_val, iter);
		if (next_field->a == NULL) rte_free(next_field->a);
		if (next_field->b == NULL) rte_free(next_field->b);
		rte_free(next_field);
	}
	rte_hash_free(old_tbl->hashmap);
	rte_free(old_tbl);
}

int _grt_redirect_table_cleanup_entry(grt_redirect_table* tbl, grt_redirect_field* field) {
	//Returns 0 if entry is valid
	//Returns 1 if entry is stale, can be ignored, but was not freed.
	//Returns 2 if entry is stale and could be freed.

	if (field->generation_a > field->generation_b) {
		if (unlikely(field->a == NULL)) {
			if (tbl->state == GRT_RT_S_IDLE) {
				//printf("Freeing field b...\n");
				if (field->b != NULL) rte_free(field->b);
				//printf("Freeing field ...\n");
				rte_free(field);
				return 2;
			} else {
				return 1;
			}
		}
	} else {
		if (unlikely(field->b == NULL)) {
			if (tbl->state == GRT_RT_S_IDLE) {
				//printf("Freeing field a, having generation %u...\n", field->generation_a);
				if (field->a != NULL) rte_free(field->a);
				//printf("Freeing field...\n");
				rte_free(field);
				return 2;
			} else {
				return 1;
			}
		}
	}
	return 0;

	
}

//Like grt_redirect_table_put, but additionally announces a GRT state update for the key that was put.
//Useful if put() is the exact state change, but not necessary to call, instead you could create your own state
//update routine.
int grt_redirect_table_put_notifystupd(grt_redirect_table* tbl, void* key, void* value, int64_t timeout, uint16_t stupd_type) {

	int result = grt_redirect_table_put(tbl, key, value, timeout);
	if (unlikely(grt_shall_notify_stupd() && result >= 0)) {

	#ifdef grt_DEBUG_REDIRTABLE
		printf("START put with active notifystupd\n");
	#endif

		uint16_t completesize = tbl->key_len + tbl->value_len + sizeof(int64_t);	//TODO: check if too large?
		void* keyvalue = rte_malloc("grt_rte_put_stupd", completesize, 0);
		rte_memcpy(keyvalue, key, tbl->key_len);
		rte_memcpy(keyvalue + tbl->key_len, value, tbl->value_len);
		rte_memcpy(keyvalue + tbl->key_len + tbl->value_len, &timeout, sizeof(int64_t));
		grt_notify_stupd(stupd_type, keyvalue, completesize);

	#ifdef grt_DEBUG_REDIRTABLE
		printf("END put with active notifystupd\n");
	#endif

	}



	return result;

}

//Also frees the state update vector.
int grt_redirect_table_put_fromstupd(grt_redirect_table* tbl, void* stupd) {
	
	void* key = rte_malloc("grt_stupd_key", tbl->key_len, 0);
	void* value = rte_malloc("grt_stupd_value", tbl->value_len, 0);
	int64_t timeout;

	rte_memcpy(key, stupd, tbl->key_len);
	rte_memcpy(value, stupd + tbl->key_len, tbl->value_len);
	rte_memcpy(&timeout, stupd + tbl->key_len + tbl->value_len, sizeof(int64_t));

	rte_free(stupd);

	int result = grt_redirect_table_put(tbl, key, value, timeout);

	rte_free(key);
	//WRONG: rte_free(value);		//Haha, THIS was the error!

	return result;

}


//Automatically frees values that are potentially replaced.
//So it is important that the value is a single struct without pointers.
//Calls between put, set and get are NOT thread safe among each other, but cope with a concurrent 
//snapshot iteration
//Returns < 0 on error.
int grt_redirect_table_put(grt_redirect_table* tbl, void* key, void* value, int64_t timeout) {

	grt_redirect_field* field;
	int retrn = rte_hash_lookup_data(tbl->hashmap, key, (void**)&field);

	uint32_t generation2Write;
	if (tbl->state == GRT_RT_S_SNAPSHOTTING) {
		generation2Write = tbl->current_generation+1;		//When writing "next" generation, 
									//it is not considered during snapshot
	} else {
		generation2Write = tbl->current_generation;		//If we are not in snapshotting, we
									//write for this generation, so it is considered.
	}

	if (likely(retrn == -ENOENT)) {
		field =rte_malloc("grt_redirect_field", sizeof(grt_redirect_field), 0);
		field->a = value;
		field->b = NULL;
		field->generation_a = generation2Write;
		field->generation_b = 0;
		field->timeout_a = timeout;
		field->timeout_b = 0;
		return rte_hash_add_key_data(tbl->hashmap, key, field);
		//Returns 0 if added successfully

	} else if (retrn >= 0) {	//TODO: DPDK Doc is wrong here! Returns >= 0! (?)
		//We pick the field with the smaller generation for overwriting, wit the exception that	
		//we need to overwrite the current generation to write (multiple writes in same gen.)
		if (field->generation_a == generation2Write || 
			field->generation_a < field->generation_b && field->generation_b != generation2Write) {
//			printf("Freeing field a...");
			if (field->a != NULL) rte_free(field->a);
			field->generation_a = generation2Write;
			field->a = value;
			field->timeout_a = timeout;
		} else {
//			printf("Freeing field b...");
			if (field->b != NULL) rte_free(field->b);
			field->generation_b = generation2Write;
			field->b = value;
			field->timeout_b = timeout;
		}
		return 1;
	} else {
		printf("Lookup in table failed. Return code: %i.\n", retrn);
		return -2;
	}

}

//Automatically frees values that are potentially replaced.
//Returns: positive value, if correctly added, a negative value 
int grt_redirect_table_remove(grt_redirect_table* tbl, void* key) {

	grt_redirect_field* field;
	int retrn = rte_hash_lookup_data(tbl->hashmap, key, (void**)&field);

	if (retrn == -ENOENT) {
		return -1; 	//Value was not in the hashtable.

	} else if (retrn >= 0) {	//TODO: DPDK Doc is wrong here! Returns >= 0! (?)
		if (likely(tbl->state == GRT_RT_S_IDLE)) {
			//Just delete the whole entry. Can safely be removed if IDLE.
			if (field->a != NULL) rte_free(field->a);
			if (field->b != NULL) rte_free(field->b);
			rte_free(field);
			return rte_hash_del_key(tbl->hashmap, key);
		} else {

			//We pick the field with the smaller generation for overwriting, wit the exception that	
			//we need to overwrite the current generation to write (multiple writes in same gen.)
		
			uint32_t generation2Write = tbl->current_generation+1;		//Only done in case of SNAPSHOTTING
			if (field->generation_a == generation2Write || 
				field->generation_a < field->generation_b && field->generation_b != generation2Write) {
				if (field->a != NULL) rte_free(field->a);
				field->generation_a = generation2Write;
				field->a = NULL;
				field->timeout_a = -1;
			} else {
				if (field->b != NULL) rte_free(field->b);
				field->generation_b = generation2Write;
				field->b = NULL;
				field->timeout_b = -1;
			}
			return 0;
		}
		
	} else {
		//printf("Lookup in table failed. Return code: %i.\n", retrn);
		return -2;
	}
}



//This is thread-safe, if called with "forSnapshot=true" AND we are in the snapshot state.
//The value is copied to the field at the specified pointer "*value".
//forSnapshot == 0, if not for snapshot, forSnapshot == something else, if for snapshot.
//Returns -1 if entry was not in the table, < -1 if an error occurred, or >= 0 on success.
int grt_redirect_table_get(grt_redirect_table* tbl, void* key, void** value, int forSnapshot) {
	
	grt_redirect_field* field = NULL;
	int retrn = rte_hash_lookup_data(tbl->hashmap, key, (void**)&field);

	if (likely(retrn == -ENOENT)) {
		//Entry was not in the table.
		return -1;

	} else if (retrn >= 0) {	//TODO: DPDK Doc is wrong here! Returns >= 0! (?)

		int retrn2 = _grt_redirect_table_cleanup_entry(tbl, field);

		if (unlikely(retrn2 == 2)) {
			rte_hash_del_key(tbl->hashmap, key);
			return -1;
		} else if (unlikely(retrn2 == 1)) {
			return -1;
		}

		uint32_t generation2ReadMax;
		if (forSnapshot == 0 && tbl->state == GRT_RT_S_SNAPSHOTTING) {
			generation2ReadMax = tbl->current_generation+1;
		} else {
			generation2ReadMax = tbl->current_generation;
		}

		if ((field->generation_a > field->generation_b || field->generation_b > generation2ReadMax)
			 && field->generation_a <= generation2ReadMax) {
			if (field->a == NULL) {
				return -1;	//Behave as entry was not in the table
			}
			*value = field->a;
			return 1;
		} else {
			//The constellation that fields are of the same generation should never happen!
			//Therefore, this field should never exceed generation2ReadMax.
			if (field->b == NULL) {
				return -1;
			}
			*value = field->b;
			return 1;
		}
	} else {
		//printf("  Lookup in table failed. Return code: %i.\n", retrn);
		return -2;
	}

	
}

//forSnapshot == 0, if not for snapshot, forSnapshot == something else, if for snapshot.
//This is thread-safe ONLY, if forSnapshot != 0!.
int grt_redirect_iterate_snapshot(grt_redirect_table* tbl, const void** key, void** data, int64_t* timeout, uint32_t* next, int forSnapshot) {

	grt_redirect_field* next_field;

	while(1) {
		//printf("  Iterating further...\n");
		int retrn = rte_hash_iterate(tbl->hashmap, key, (void**)&next_field, next);
		//printf("  Iteration finished, return value = %i", retrn);		

		if (retrn >= 0) {

			uint32_t generation2ReadMax;
			if (forSnapshot == 0 && tbl->state == GRT_RT_S_SNAPSHOTTING) {
				generation2ReadMax = tbl->current_generation+1;
			} else {
				generation2ReadMax = tbl->current_generation;
			}

			if ((next_field->generation_a > next_field->generation_b || next_field->generation_b > generation2ReadMax)
				 && next_field->generation_a <= generation2ReadMax) {

				//printf("  Writing next_field->a to data...");
				*data = next_field->a;
				*timeout = next_field->timeout_a;

			} else {
				//The constellation that fields are of the same generation should never happen!
				//Therefore, this field should never exceed generation2ReadMax.

				//printf("  Writing next_field->b to data...");
				*data = next_field->b;
				*timeout = next_field->timeout_b;
				
			}
			if (*data != NULL) return retrn;	//If data is NULL, entry was deleted in an OLDER snapshot.
							//So go on with iterating.

		} else {
			return retrn;
		}
	}

}

void grt_redirect_set_snap_state(grt_redirect_table* tbl, int future_state) {
	if (tbl->state == GRT_RT_S_SNAPSHOTTING && future_state == GRT_RT_S_IDLE) {
		tbl->state = GRT_RT_S_IDLE;
		tbl->current_generation++;
	} else if (tbl->state == GRT_RT_S_IDLE && future_state == GRT_RT_S_SNAPSHOTTING) {
		tbl->state = GRT_RT_S_SNAPSHOTTING;
	}
}



int grt_redirect_dump_debug(grt_redirect_table* tbl) {

	int32_t iter = 0;
	void* next_key;
	grt_redirect_field* next_field;
	int32_t return_val;

	while (return_val = rte_hash_iterate(tbl->hashmap, (const void**)&next_key, (void**)&next_field, &iter) >= 0) {
		printf("Entry: k=%p => (a=%p, gen_a=%u, b=%p, gen_b=%u)\n",
			next_key, next_field->a, next_field->generation_a, next_field->b, next_field->generation_b);
	}

	return 0;

}

//MUST be in snapshotting state if we do this.
//Warning: At the end, this skips back to the memory and writes the total length into the first 8 bytes.
//So cannot be put into a stream.
//TODO: Required size to malloc() can be very large, so evaluate if we need a malloc in hugepages space!
//Returns the pointer to the buf directly AFTER the serialized stream.
//maxOffset is *exclusive*, and is a limit until which we can write at maximum.

int grt_redirect_serialize_snapshot(grt_redirect_table* tbl, void** offset, void* maxOffset) {

	if (tbl->state != GRT_RT_S_SNAPSHOTTING) return -EINVAL;

	void* originalOffset = *offset;

	int32_t iter = 0;
	void* next_key;
	void* next_value;
	int64_t next_timeout;
	int32_t return_val;

	*offset += sizeof(uint64_t);	//for the length descriptor.

	uint32_t entry_len = tbl->key_len + tbl->value_len + sizeof(int64_t);

	int result = 0;

	//printf("Starting to iterate, offset %p\n", *offset);

	uint64_t i = 0;
	while (return_val = grt_redirect_iterate_snapshot(tbl, (const void**)&next_key, &next_value, &next_timeout, &iter, 1) >= 0) {
		
		if (*offset + entry_len > maxOffset) {
			result = -1;
			break;
		}
		//printf("Copying to %p from %p, size %i\n", *offset, next_key, tbl->key_len);
		rte_memcpy(*offset, next_key, tbl->key_len);
		*offset += tbl->key_len;

		//printf("Copying to %p from %p, size %i\n", *offset, next_value, tbl->value_len);
		rte_memcpy(*offset, next_value, tbl->value_len);
		*offset += tbl->value_len;

		//printf("Copying to %p from %p, size %lu\n", *offset, &next_timeout, sizeof(int64_t));
		rte_memcpy(*offset, &next_timeout, sizeof(int64_t));
		*offset += sizeof(int64_t);

		i++;

	}

	rte_memcpy(originalOffset, &i, sizeof(uint64_t));

	return result;

}

//Deserializes a snapshot. If tbl is not empty, entries are merged, potential duplicates are overwritten.
int grt_redirect_deserialize_snapshot(grt_redirect_table* tbl, void** offset) {

	if (tbl->state != GRT_RT_S_IDLE) return -EINVAL;

	uint64_t num_entries;
	rte_memcpy(&num_entries, *offset, sizeof(uint64_t));
	
	*offset += sizeof(uint64_t);

	void* key = rte_malloc("serialize_key", tbl->key_len, 0);		//Do that with buffer instead? Must be variable size..

	for (uint64_t i=0; i < num_entries; i++) {

		rte_memcpy(key, *offset, tbl->key_len);
		*offset += tbl->key_len;

		void* value = rte_malloc("serialize_value", tbl->value_len, 0);
		rte_memcpy(value, *offset, tbl->value_len);
		*offset += tbl->value_len;

		int64_t timeout;
		rte_memcpy(&timeout, *offset, sizeof(int64_t));
		*offset += sizeof(int64_t);

		grt_redirect_table_put(tbl, key, value, timeout);


	}

	rte_free(key);

	return 0;
	

}


























