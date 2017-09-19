/*
Simple mobile packet gateway NF which maintains current cell associations as a state.
When a client switches cells, the client announces this to the PG in a packet 
header and the NF keeps track of it, so that returning packets from the core network 
are directed to the respective cell.

Intended as a simple example of a NF using SliM for testing its quick reaction to
time-critical events. Can be used as a template for implementing more NFs.

This implementation makes use of the GRT redirect table, which has built-in support
for statelet announcement/installation.
*/

#include <stdio.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_string_fns.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_jhash.h>

#include <byteswap.h>
#include <grt_main.h>
#include <grt_toolz.h>
#include <grt_redirect_table.h>

#define CACHE_LINE SIZE		64
#define MIN_EXT_PORT		1024
#define SNAPSHOT_BUF_SIZE	4*1024*1024	// 4MB

//#define ERRONEOUS_PACKET_FAILURES		//Otherwise, silent discard. Not useful for high bandwidths.

//#define grt_DEBUG
//#define grt_DEBUG_DUMP_STATE
//#define DEBUG_HST_SEQUENCE
#define grt_DEBUG_SNAPSHOTS
//#define grt_DEBUG_STUPDS

#define GRT_STATELET_TYPE_NEW_HANDOVER	10
#define PREAMBLE		0xd066f00d




#define TYPE_CELLASSOC_KEY 102;
typedef struct  __attribute__((__packed__)) {
	uint32_t subsc_clIP;
	uint16_t subsc_clPort;
} cellassoc_key;

typedef struct  __attribute__((__packed__)) {
	uint16_t cell_id;
	char bogus_info [42];	//Resembles some additional handover data for the statelet.
} cellassoc_value;


typedef struct __attribute__((__packed__)) {
	uint32_t preamble;
	uint16_t stream_id;
	uint32_t packet_c;
	uint64_t timestamp;
	uint16_t cellId;
	uint8_t update;
} in_packet_info_t;

grt_redirect_table* cellassoc_table = NULL;

int packet_c = 0;

void memPrintHex(void* start, char* preamble, char* postamble, uint32_t len) {
	printf("%s", preamble);
	int i;
	for(i=0; i<len;i++) printf("%02x ", ((unsigned char*)start)[i]);
	printf("%s", postamble);
}

// Prints out the state of the hash table, just for debugging.
void fancyDumpState() {
	int32_t iter = 0;
	int32_t return_val;
	int64_t next_timeout;
	printf("/========START ASSOC TABLE========\\\n");
	cellassoc_key* next_key2;
	cellassoc_value* next_value2;
	char ip_str_buf[20];
	while (return_val = grt_redirect_iterate_snapshot(cellassoc_table, (const void**)&next_key2, (void**)&next_value2, &next_timeout, &iter, 0) >= 0) {
		grt_ipv4_to_string(ip_str_buf, next_key2->subsc_clIP);
		printf(" (ip=%s, port=%u) => (%u), return_val=%i, iter=%i.\n", ip_str_buf, next_key2->subsc_clPort, next_value2->cell_id, return_val, iter); 
	}
	printf("\\========END ASSOC TABLE========/ Final return value: %u\n", return_val);



}

//Obtains the current association entry for the client. If it 
//needs to be changed, a new statelet is created.
int _getWrAssocEntryFromIntPacket(cellassoc_value** resultEntry, cellassoc_key* key, in_packet_info_t* payload) {

	int retrn = grt_redirect_table_get(cellassoc_table, (void*)key, (void**)resultEntry, 0);

	if (retrn == 1 && payload->update == 0) {	
		//Cell ID does not need to be updated.
		return 0;
	}
	
	int old_assoc = (retrn == 1)?(*resultEntry)->cell_id:-1;

	*resultEntry = rte_malloc("CELLASSOC_VALUE", sizeof(cellassoc_value), 0);
		//In either case we need a new mem struct, do never overwrite the old one for snapshotting purposes!!!
	
	(*resultEntry)->cell_id = payload->cellId;

	retrn = grt_redirect_table_put_notifystupd(cellassoc_table, (void*)key, (*resultEntry), -1, GRT_STATELET_TYPE_NEW_HANDOVER);
	
#ifdef grt_DEBUG_STUPDS
	RTE_LOG(INFO, USER1, "Would have generated statelet. New cell:%u, old cell:%i.\n", (*resultEntry)->cell_id, old_assoc);
#endif
	
	if (retrn < 0) {
		RTE_LOG(INFO, USER1, "    ERROR: Could not insert new association. \n");
	}

}


int _prepare_normal_ipv4(struct rte_mbuf* packet, struct ipv4_hdr* ip_hdr, struct udp_hdr* l4hdr, uint8_t l4proto) {

	if (l4proto == 17) {
		l4hdr->dgram_cksum = 0;
		l4hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, l4hdr);
	} else {
		//next_proto == 6
		
		((struct tcp_hdr*)l4hdr)->cksum = 0;
		((struct tcp_hdr*)l4hdr)->cksum = rte_ipv4_udptcp_cksum(ip_hdr, l4hdr);
	}

	ip_hdr->hdr_checksum = 0;
	ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

}


//Handles an incoming IPv4 packet.
int handle_ipv4(uint16_t vlan_offset, struct rte_mbuf* packet, uint8_t grt_if_id) {

	struct ether_hdr* eth_hdr = rte_pktmbuf_mtod_offset(packet, struct ether_hdr*, 0);
	struct ipv4_hdr* ip_hdr = rte_pktmbuf_mtod_offset(packet, struct ipv4_hdr*, sizeof(struct ether_hdr) + vlan_offset);
	
	uint8_t version = (ip_hdr->version_ihl & ~IPV4_HDR_IHL_MASK) >> 4;

	if (unlikely(version != 4)) {
		RTE_LOG(INFO, USER1, "Cannot handle IP protocol version %u", version);
		return -1;
	}

	uint8_t hdr_len = (ip_hdr->version_ihl & IPV4_HDR_IHL_MASK) * IPV4_IHL_MULTIPLIER;

	//uint8_t 	ip_hdr->time_to_live;
	//uint8_t	ip_hdr->next_proto_id;	//ICMP=1, UDP=17, TCP=6

	//RTE_LOG(INFO, USER1, "IP packet info version=%u, hdr_len=%u, ttl=%u, next_proto=%u\n", version, hdr_len, ttl, next_proto);

	uint32_t ip_src = __bswap_32(ip_hdr->src_addr);
	uint32_t ip_dst = __bswap_32(ip_hdr->dst_addr);

	uint8_t next_proto = ip_hdr->next_proto_id;

	if (unlikely(next_proto != 17)  ) {
		if (next_proto == 1) RTE_LOG(INFO, USER1, " ICMP is not supported.\n");
		else RTE_LOG(INFO, USER1, " Cannot handle something different than UDP. Proto ID was: %u\n", next_proto);
		return -1;
	}

	struct udp_hdr* udphdr = rte_pktmbuf_mtod_offset(packet, struct udp_hdr*, sizeof(struct ether_hdr) + vlan_offset + sizeof(struct ipv4_hdr));
	uint16_t port_src = __bswap_16(udphdr->src_port);
	uint16_t port_dst = __bswap_16(udphdr->dst_port);

#ifdef grt_DEBUG

	char ip_src_str_buf[20];
	char ip_dst_str_buf[20];
	grt_ipv4_to_string(ip_src_str_buf, ip_src);
	grt_ipv4_to_string(ip_dst_str_buf, ip_dst);

	RTE_LOG(INFO, USER1, "Got a packet: src=%s, dst=%s.\n", ip_src_str_buf, ip_dst_str_buf);
#endif
	

	in_packet_info_t* payload = rte_pktmbuf_mtod_offset(packet, in_packet_info_t*, sizeof(struct ether_hdr) + vlan_offset + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr));
	
	if (unlikely(payload->preamble != __bswap_32(PREAMBLE))) {
		RTE_LOG(INFO, USER1, " Wrong preamble received. Cannot process. Preamble was: %x.\n", payload->preamble);
		return -2;
	}

	if (grt_if_id == 0) {
	  
		//Packet came from interior network.

		cellassoc_key key;
		key.subsc_clIP = ip_src;
		key.subsc_clPort = port_src;

		cellassoc_value* lookupEntry = NULL;
		int retrn = _getWrAssocEntryFromIntPacket(&lookupEntry, &key, payload);
		
		payload->cellId = 0;	//Means: Unknown in the "wild".


#ifdef grt_DEBUG_DUMP_STATE
		fancyDumpState();
#endif

	}

	if (grt_if_id == 1) {



		cellassoc_key key;
		key.subsc_clIP = ip_dst;
		key.subsc_clPort = port_dst;


#ifdef grt_DEBUG_DUMP_STATE
		fancyDumpState();
		printf(" Looking for assoc key (subsc_clIP=%u, subsc_clPort=%hu)\n", key.subsc_clIP, key.subsc_clPort);
#endif


		cellassoc_value* lookupEntry = NULL;
		int retrn = grt_redirect_table_get(cellassoc_table, &key, (void**)&lookupEntry, 0);


		if (unlikely(retrn == -1)) {
			//Not in table. Drop packet.
			RTE_LOG(INFO, USER1, "No assoc. Was looking for Assoc key (subsc_clIP=%u, subsc_clPort=%hu).\n", key.subsc_clIP, key.subsc_clPort);
			return 0;
		}
		if (unlikely(retrn != 1)) {
			RTE_LOG(INFO, USER1, " ERROR while querying assoc table, return code %i.\n", retrn);
			return -1;
		}

#ifdef grt_DEBUG

		if (lookupEntry == NULL) {
			printf(" Assoc entry was NULL, although correctly returned. Should never happen %i.\n", retrn);
			return -1;
		}

		printf(" Found assoc entry (subsc_clIP=%u, subsc_clPort=%hu) -> (cell_id=%hu)\n", key.subsc_clIP, key.subsc_clPort, lookupEntry->cell_id);

#endif
		
		payload->cellId = lookupEntry->cell_id;
		payload->update = 2;

		

	}
	
	return _prepare_normal_ipv4(packet, ip_hdr, udphdr, next_proto);

	return -1;

}

// Remember to free IF NOT SENT!!!
int handle_packet(struct ether_hdr* l2hdr, struct rte_mbuf* packet, uint8_t grt_if_id) {

	struct ipv4_hdr *ipv4_hdr;
	ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(packet, unsigned char *) + sizeof(struct ether_hdr));
		//Get the beginning of the packet's mbuf and skip for the size of an ether header.


#ifdef grt_DEBUG
	char saddr_str [20];
	char daddr_str [20];
	char ethtype_str [8];


	
	grt_ethaddr_to_string(saddr_str, &l2hdr->s_addr);
	grt_ethaddr_to_string(daddr_str, &l2hdr->d_addr);
	grt_ethertype_to_string(ethtype_str, l2hdr->ether_type);

	RTE_LOG(INFO, USER1, "pktIn: packet_c=%i, L2 src=%s, dest=%s, ethertype=%s\n",
		packet_c++,
		saddr_str, 
		daddr_str,
		ethtype_str
		);
	
#endif

	if (likely(l2hdr->ether_type == 0x0008)) {
#ifdef grt_DEBUG
		RTE_LOG(INFO, USER1, "IPv4 packet received at IF %u.\n", grt_if_id);
#endif

		//Handle the IPv4 packet
		handle_ipv4(0, packet, grt_if_id);

	}
	
	if (likely(grt_shall_output() > 0)) {
	
	      uint8_t grt_if_2snd = (grt_if_id == 1)?0:1;
	      
	      struct grt_interface_info* if_info = grt_getInterfaceInfo(grt_if_2snd);
	      struct rte_mbuf* pkt2send_buf [1];
	      pkt2send_buf[0] = packet;	//TODO can this be made in bulks????
	      int retrn = rte_eth_tx_burst(if_info->rte_port_id, 0, pkt2send_buf, 1);
	      if (retrn == 1) 
		      return 1;
	      else {
#ifdef ERRONEOUS_PACKET_FAILURES
		      printf("    TX burst failed with error code %i.\n", retrn);
#endif
		      return -1;
	      }
	}
	
	return 0;

}

//Packs the current state for sending it via the SliM snapshot stream.
int handle_snapshot_out() {

	grt_redirect_set_snap_state(cellassoc_table, GRT_RT_S_SNAPSHOTTING);

	void* snapBuf = rte_zmalloc("SNAPSHOT_OUT_BUFFER", SNAPSHOT_BUF_SIZE, 0);
	void* snapBufOffsetMax = snapBuf + SNAPSHOT_BUF_SIZE;
	void* snapBufOffset = snapBuf;

#ifdef grt_DEBUG_SNAPSHOTS
	void* offsetBefore = snapBufOffset;
#endif
	int retrn = grt_redirect_serialize_snapshot(cellassoc_table, &snapBufOffset, snapBufOffsetMax);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Serialized assoc table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, snapBufOffset, snapBufOffset-offsetBefore, retrn);
	RTE_LOG(INFO, USER1, "Starting snapshot transfer now...\n");
#endif

	//Sending the buffered snapshot from here...

	grt_addDataToSnapshot(snapBuf, snapBufOffset-snapBuf);

	if (conf_artificialsnapsz > 0) {
		uint32_t i;
		uint8_t buf[4096];
		bzero(buf, 4096);
		for (i=0; i < conf_artificialsnapsz; i++) {
			grt_addDataToSnapshot(buf, 4096);
		}
	}

	RTE_LOG(INFO, USER1, "Snapshot transfer finished.\n");
	rte_free(snapBuf);

	grt_redirect_set_snap_state(cellassoc_table, GRT_RT_S_IDLE);

	return 0;

}

//Called whenever a statelet has been received which must be installed.
//SliM guarantees this is not interrupted by the snapshot install process or packet-in events.
int handle_state_update(uint16_t type, void* stupd_vec, uint16_t len) {

	if (type == GRT_STATELET_TYPE_NEW_HANDOVER) {

		grt_redirect_table_put_fromstupd(cellassoc_table, stupd_vec);
		return 1;
	} else {
		//Unknown state update type
		rte_free(stupd_vec);
	}
	

	//printf("VNF got state update: type=%u, string='%s', len=%u\n", type, (char*)buf4Snapshots, lenOfBuf);
	return 0;

}

//Installs the snapshot. This is guaranteed not to be disrupted by any pktIn or state update packet,
//until the function returns.
int handle_snapshot_in() {
	//TODO: Table must be cleared IF NOT merge.

	void* snapBuf = rte_malloc("SNAPSHOT_IN_BUFFER", SNAPSHOT_BUF_SIZE, 0);
	ptrdiff_t snapSize = grt_getDataFromSnapshot(snapBuf, SNAPSHOT_BUF_SIZE);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Received a snapshot of size %li.\n", snapSize);
#endif

	void* currentOffset = snapBuf;

#ifdef grt_DEBUG_SNAPSHOTS
	void* offsetBefore = currentOffset;
#endif
	//TODO: Fix potential security hole: Pointer to end of table snapshot in deserialize string may cause a buffer overflow!!!
	int retrn = grt_redirect_deserialize_snapshot(cellassoc_table, &currentOffset);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Deserialized cellassoc table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, currentOffset, currentOffset-offsetBefore, retrn);
	offsetBefore = currentOffset;
#endif

	RTE_LOG(INFO, USER1, "Snapshot receive finished.\n");

	rte_free(snapBuf);

#ifdef grt_DEBUG_DUMP_STATE
	fancyDumpState();
#endif

	return 0;

}




int prepare() {
  //Nothing to do
}

int prepareInEAL() {
	static struct rte_hash_parameters defaultParamsCellassoc = {
		.name = "GRT_CELLASSOC_TABLE",
		.entries = 65536,
		.reserved = 0,
		.key_len = sizeof(cellassoc_key),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
		.socket_id = 0,
		.extra_flag = 0,
	};


	printf("[App] Creating tables...\n");

	printf("[App] sizeof(cellassoc_key)=%lu.\n", sizeof(cellassoc_key));

	cellassoc_table = grt_redirect_table_create(&defaultParamsCellassoc, sizeof(cellassoc_key), sizeof(cellassoc_value));

	printf("[App] Created assoc table.\n");

	return 23;

}


int main(int argc, char **argv) {

	prepare();

	printf("Starting Handover app for SliM...\n");
	grt_main(argc, argv, &prepareInEAL, &handle_packet, &handle_snapshot_out, &handle_snapshot_in, &handle_state_update);

}

