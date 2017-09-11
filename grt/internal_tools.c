#include <inttypes.h>

#define grt_astrcpy(dest,src) strcpy(dest=malloc(strlen(src)+1),src)

static unsigned tz_getMBufMempoolSize(uint8_t ports_c, uint8_t lcores_c, uint8_t rx_queues_c, uint8_t tx_queues_c) {

	unsigned result =
	ports_c * rx_queues_c * RTE_TEST_RX_DESC_DEFAULT + 
	ports_c * lcores_c * MAX_SIZE_BURST + 
	ports_c * tx_queues_c * RTE_TEST_TX_DESC_DEFAULT + 
	lcores_c * MEMPOOL_CACHE_SIZE;

	if (result < 8192) return 8192;
	return result;

}

