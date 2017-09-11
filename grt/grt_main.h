/*
* Interface to the GRT app 
*/

#include <rte_ether.h>

#define grt_MAX_STUPD_LEN  	2048
#define GRT_STUPD_TYPE_WHOLEPACKET	1336

uint32_t conf_artificialsnapsz;	//Just to know if artificial snapshots are generated.

struct grt_lcore_info_s {
	struct rte_mempool* mempool; /**< Mempool to be used */
	uint8_t  lcore_id;       /**< Our own lcore ID. Redundant to info in fwd_available_lcores[..] */
	struct rte_ring* waitbuf;
	uint64_t debug;
};

struct grt_interface_info {
	uint8_t rte_port_id;
	uint8_t grt_intf_id;
	struct ether_addr intf_mac;
};

typedef int (*prepareInEALPtr)();

//Called if a packet was received from DPDK.
typedef int (*handlePktPtr)(struct ether_hdr*, struct rte_mbuf*, uint8_t);

//Called if a state update was received
typedef int (*handleStupdPtr)(uint16_t, void*, uint16_t);

//Called if a snapshot of the state has to be taken.
//Calls grt_addDataToSnapshot(...) one or multiple times
typedef int (*handleSnOutPtr)();

//Called if a snapshot of the state must be installed.
//grt_getDataFromSnapshot(...) one or multiple times.
typedef int (*handleSnInPtr)();

//Returns GRT information from a DPDK interface.
extern struct grt_interface_info* grt_getInterfaceInfo(uint8_t);

//Returns GRT-specific information from the currently running lcore.
extern struct grt_lcore_info_s* grt_getCurrentLCoreInfo();

//Initiates EAL and registers the callbacks.
extern int grt_main(int, char **, prepareInEALPtr, handlePktPtr, handleSnOutPtr, handleSnInPtr, handleStupdPtr);


//======== State Management



//While creating the snapshot, this function adds data to it. Blocks, until this part of the snapshot data has been transferred.
//Must ONLY be called inside the function on the pointer *handleSnOutPtr.

// void*		a pointer to the bytes.
// ptrdiff_t		the length of its bytes.
extern int grt_addDataToSnapshot(void* data, ptrdiff_t length);


//Returns whether the VNF shall currently 
//send state updates. returns > 0, if yes
extern int grt_shall_notify_stupd();


//Notifies about a state update in a byte vector representation
//Note that the byte vector must be alloc'ed (not on the stack),
//grt will free it after use! Immediately returns, is multithread-safe.
//
// void*	pointer to a buffer with the byte vector.
// uint16_t	the length of the byte vector.
extern int grt_notify_stupd(uint16_t type, void* byteVectorObj, uint16_t length);


//While receiving the snapshot, this function receives data for it. Blocks, until there is more data available.
//Must ONLY be called inside the function on the pointer *handleSnInPtr.

//This returns 0, if the snapshot has been completely transferred.

// void*		a pointer to the buffer to write into.
// ptrdiff_t		the maximum bytes to write to the buf.
extern ptrdiff_t grt_getDataFromSnapshot(void*, ptrdiff_t);

//TODO
extern int grt_shall_output();

extern uint8_t is_macfilterfix_enabled();	//Is 1 if enabled.






