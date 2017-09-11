#include <rte_common.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <errno.h>

#include "../grt_redirect_table.h"

#define DEBUG_TEXT 		"dbg1"
#define SERIALIZE_BUF_SIZE 	65536

typedef struct  {
	uint32_t a;
	uint32_t b;
} test_key;

typedef struct  {
	uint32_t c;
	uint32_t d;
} test_value;

static struct rte_hash_parameters defaultParams = {
	.name = "Test1_0",
	.entries = 64,
	.reserved = 0,
	.key_len = sizeof(test_key),
	.hash_func = rte_jhash,
	.hash_func_init_val = 0,
	.socket_id = 0,
	.extra_flag = 0,
};

void test_iterateAndPrint(struct rte_hash* table2Iterate) {
	int32_t iter = 0;
	test_key* next_key;
	test_value* next_value;
	int32_t return_val;
	while (return_val = rte_hash_iterate(table2Iterate, (const void**)&next_key, (void**)&next_value, &iter) >= 0) {
		printf("Found: (%u, %u) -> (%u, %u), return_val=%i, iter=%i.\n", next_key->a, 
			next_key->b, next_value->c, next_value->d, return_val, iter);
	}
	printf("Iteration Return value: %u\n", return_val);
	
	
}

int putTestEntry(grt_redirect_table* tbl, uint32_t keya, uint32_t keyb, uint32_t valc, uint32_t vald) {
	printf("Putting entry: (%u,%u,%u,%u)\n", keya, keyb,valc,vald);	
	test_key key;
	key.a = keya;
	key.b = keyb;
	test_value* value1 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value1->c = valc;
	value1->d = vald;
	int retrn = grt_redirect_table_put(tbl, &key, value1, -1);
	printf("  addTestEntry, return code: %i\n", retrn);
}

int getTestEntry(grt_redirect_table* tbl, uint32_t keya, uint32_t keyb) {
	printf("Looking for entry at: (%u,%u)\n", keya, keyb);
	test_key key;
	key.a = keya;
	key.b = keyb;
	test_value* valueReturned = NULL;
	int retrn = grt_redirect_table_get(tbl, &key, (void**)&valueReturned, 0);
	printf("  getTestEntry, return code: %i\n", retrn);
	if (retrn == 1 && valueReturned != NULL) {
		printf("  Returned entry: (%u,%u)\n", valueReturned->c, valueReturned->d);
	}
}

int removeTestEntry(grt_redirect_table* tbl, uint32_t keya, uint32_t keyb) {
	printf("Removing for entry at: (%u,%u)\n", keya, keyb);
	test_key key;
	key.a = keya;
	key.b = keyb;
	int retrn = grt_redirect_table_remove(tbl, &key);
	printf("  removeTestEntry, return code: %i\n", retrn);
	if (retrn >= 0) {
		printf("  Process returns: successfully deleted.\n");
	}
}

int makeAndPrintSnapshot(grt_redirect_table* tbl) {
	int32_t iter = 0;
	test_key* next_key;
	test_value* next_value;
	int64_t next_timeout;
	int32_t return_val;
	printf("/=== Snapshot Begin\n");
	while (return_val = grt_redirect_iterate_snapshot(tbl, (const void**)&next_key, (void**)&next_value, &next_timeout, &iter, 1) >= 0) {
		printf("  (%u, %u) -> (%u, %u, to=%li), iter=%i. return_val=%i\n", next_key->a, 
			next_key->b, next_value->c, next_value->d, next_timeout, iter, return_val);
	}
	printf("\\=== Snapshot End\n");
}

void test_test2() {

	printf("Creating table...\n");
	grt_redirect_table* tbl = grt_redirect_table_create(&defaultParams, sizeof(test_key), sizeof(test_value));
	
	putTestEntry(tbl, 1,2,3,4);
	putTestEntry(tbl, 5,6,7,8);
	putTestEntry(tbl, 9,10,11,12);
	getTestEntry(tbl, 5,6);

	removeTestEntry(tbl, 5,6);
	getTestEntry(tbl, 5,6);

	removeTestEntry(tbl, 11,12);

	grt_redirect_dump_debug(tbl);

	printf("Freeing table...\n");
	grt_redirect_table_free(tbl);

}





void test_test3_serialization(grt_redirect_table** tbl) {

	printf("Testing serialization...\n");

	makeAndPrintSnapshot(*tbl);

	grt_redirect_set_snap_state(*tbl, GRT_RT_S_SNAPSHOTTING);

	void* buf = rte_malloc("Serialize Buffer", SERIALIZE_BUF_SIZE, 0);
	void* bufOffsetWrite = buf;
	void* maxBufOffset = buf + SERIALIZE_BUF_SIZE;

	printf("buf: %p, bufOffset: %p\n", buf, bufOffsetWrite);
	int retrn = grt_redirect_serialize_snapshot(*tbl, &bufOffsetWrite, maxBufOffset);

	printf("Wrote %li bytes for serialization, return value %i\n", bufOffsetWrite - buf, retrn);

	grt_redirect_set_snap_state(*tbl, GRT_RT_S_IDLE);


	printf("Freeing table...\n");
	grt_redirect_table_free(*tbl);


	*tbl = grt_redirect_table_create(&defaultParams, sizeof(test_key), sizeof(test_value));	
	void* bufOffsetRead = buf;

	retrn = grt_redirect_deserialize_snapshot(*tbl, &bufOffsetRead);
	printf("Read %li bytes for serialization, return value %i\n", bufOffsetRead - buf, retrn);

	makeAndPrintSnapshot(*tbl);

}





void test_test3() {
	printf("Creating table...\n");

	grt_redirect_table* tbl = grt_redirect_table_create(&defaultParams, sizeof(test_key), sizeof(test_value));

	putTestEntry(tbl, 1,2,3,4);
	putTestEntry(tbl, 5,6,7,8);
	putTestEntry(tbl, 9,10,11,12);




	printf("Setting to Snapshotting...\n");
	grt_redirect_set_snap_state(tbl, GRT_RT_S_SNAPSHOTTING);

	makeAndPrintSnapshot(tbl);

	putTestEntry(tbl, 101,102,103,104);
	putTestEntry(tbl, 105,106,107,108);
	putTestEntry(tbl, 5,6,107,108);
	
	makeAndPrintSnapshot(tbl);

	printf("Unsetting snapshotting...\n");
	grt_redirect_set_snap_state(tbl, GRT_RT_S_IDLE);





	makeAndPrintSnapshot(tbl);
	//grt_redirect_dump_debug(tbl);

	putTestEntry(tbl, 105,106,107,108);
	putTestEntry(tbl, 1,2,1007,1008);
	removeTestEntry(tbl, 1,2);

	makeAndPrintSnapshot(tbl);
	//grt_redirect_dump_debug(tbl);




	printf("Setting to Snapshotting...\n");
	grt_redirect_set_snap_state(tbl, GRT_RT_S_SNAPSHOTTING);

	makeAndPrintSnapshot(tbl);

	putTestEntry(tbl, 1001,1002,1003,1004);
	removeTestEntry(tbl, 9,10);
	
	makeAndPrintSnapshot(tbl);

	printf("Unsetting snapshotting...\n");
	grt_redirect_set_snap_state(tbl, GRT_RT_S_IDLE);


	makeAndPrintSnapshot(tbl);

	
	test_test3_serialization(&tbl);

	printf("Freeing table...\n");
	grt_redirect_table_free(tbl);
	
}

void test_test1() {

	printf("Starting...\n");

	

	printf("Creating testtable...\n");

	struct rte_hash* testtable = rte_hash_create(&defaultParams);

	printf("Inserting key 1\n");

	test_key key;
	key.a = 1;
	key.b = 2;
	test_value* value1 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value1->c = 3;
	value1->d = 4;
	rte_hash_add_key_data(testtable, &key, value1);

	printf("Inserting key 2\n");

	key.a = 5;
	key.b = 6;
	test_value* value2 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value2->c = 7;
	value2->d = 8;
	rte_hash_add_key_data(testtable, &key, value2);

	printf("Inserting key 3\n");

	key.a = 9;
	key.b = 10;
	test_value* value3 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value3->c = 11;
	value3->d = 12;
	rte_hash_add_key_data(testtable, &key, value3);

	printf("Inserting key 4\n");

	key.a = 13;
	key.b = 14;
	test_value* value4 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value4->c = 15;
	value4->d = 16;
	rte_hash_add_key_data(testtable, &key, value4);

	printf("Inserting key 5\n");

	key.a = 17;
	key.b = 18;
	test_value* value5 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value5->c = 19;
	value5->d = 20;
	rte_hash_add_key_data(testtable, &key, value5);



	printf("Now iterate...\n");

	test_iterateAndPrint(testtable);


	printf("Testing iteration while writing...\n");


	int32_t iter = 0;
	test_key* next_key;
	test_value* next_value;
	int32_t return_val;


	for (int i = 0; i < 2; i++) {	
		return_val = rte_hash_iterate(testtable, (const void**)&next_key, (void**)&next_value, &iter);
		printf("Found: (%u, %u) -> (%u, %u), return_val=%i, iter=%i.\n", next_key->a, 
				next_key->b, next_value->c, next_value->d, return_val, iter);
	}

	return_val = rte_hash_iterate(testtable, (const void**)&next_key, (void**)&next_value, &iter);
	printf("Found: (%u, %u) -> (%u, %u), return_val=%i, iter=%i.\n", next_key->a, 
			next_key->b, next_value->c, next_value->d, return_val, iter);

	printf("Inserting key while iterating...\n");
		//This should work for GRT redirect-on-write. It does not matter if intermediately inserted keys show up 
		//during the iteration, or not. I suppose intermediate deletion does not work without errors, however we 
		//don't do it!

	key.a = 100;
	key.b = 101;
	test_value* value6 = (test_value*)rte_malloc(DEBUG_TEXT, sizeof(test_value), 0);
	value6->c = 102;
	value6->d = 103;
	rte_hash_add_key_data(testtable, &key, value6);

	while (return_val = rte_hash_iterate(testtable, (const void**)&next_key, (void**)&next_value, &iter) >= 0) {	

		printf("Found: (%u, %u) -> (%u, %u), return_val=%i, iter=%i.\n", next_key->a, 
				next_key->b, next_value->c, next_value->d, return_val, iter);
	}


	rte_free(value1);
	rte_free(value2);
	rte_free(value3);
	rte_free(value4);
	rte_free(value5);
	rte_free(value6);
	rte_hash_free(testtable);


}

void test_main() {
	
	printf("Error codes: ENOENT=%i, EINVAL=%i", ENOENT, EINVAL);

	//for (uint32_t i = 0; i < 20; i++) {
	//	printf("=============== Test %u\n", i);
	//	defaultParams.hash_func_init_val=i;
	//	test_test1();
	//}

	test_test3();
}




























