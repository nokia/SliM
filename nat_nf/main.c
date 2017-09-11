
/*
* Example NAT network function using SliM.
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
#define grt_DEBUG_STUPDS

#define GRT_STUPD_TYPE_NEW_NATENTRY		10
#define GRT_STUPD_TYPE_NEW_ARPENTRY		11
#define GRT_STUPD_TYPE_HOSTS_COUNTERCHANGE	12
#define GRT_STUPD_TYPE_HST_UPDATE_IN		13
#define GRT_STUPD_TYPE_HST_UPDATE_OUT		14


typedef struct  __attribute__((__packed__)) {
	uint32_t int_clIP;
	uint32_t ext_clIP;
	uint32_t srvIP;
	uint16_t int_clPort;
	uint16_t ext_clPort;
	uint16_t srvPort;
} nat_stupd_entry;

#define TYPE_NAT_KEY_IN2OUT 101
typedef struct  __attribute__((__packed__)) {
	uint32_t int_clIP;
	uint32_t srvIP;
	uint16_t int_clPort;
	uint16_t srvPort;
	uint8_t type;	//Must be set to TYPE_NAT_KEY_IN2OUT;
} nat_key_in2out;

typedef struct  __attribute__((__packed__)) {
	uint32_t ext_clIP;
	uint16_t ext_clPort;
} nat_value_in2out;

#define TYPE_NAT_KEY_OUT2IN 102;
typedef struct  __attribute__((__packed__)) {
	uint32_t ext_clIP;
	uint32_t srvIP;
	uint16_t ext_clPort;
	uint16_t srvPort;
	uint8_t type; //Must be set to TYPE_NAT_KEY_OUT2IN;
} nat_key_out2in;

typedef struct  __attribute__((__packed__)) {
	uint32_t int_clIP;
	uint16_t int_clPort;
} nat_value_out2in;

typedef struct  __attribute__((__packed__)) {
	uint32_t ip_addr;
} arptable_key;

typedef struct  __attribute__((__packed__)) {
	struct ether_addr mac_addr;
} arptable_value;

typedef struct  __attribute__((__packed__)) {
	uint32_t int_clIP;
} hsttable_key;

typedef struct  __attribute__((__packed__)) {
	uint64_t bytecount;
} hsttable_value;

grt_redirect_table* nat_table = NULL;
grt_redirect_table* arp_table = NULL;
grt_redirect_table* hst_table_in = NULL;
grt_redirect_table* hst_table_out = NULL;

int packet_c = 0;

uint32_t interior_ipv4_addr;
uint32_t interior_ipv4_mask;

uint32_t exterior_ipv4_addr;
uint32_t exterior_ipv4_mask;

uint32_t exterior_defaultgateway;

struct ether_addr interior_dev_mac;
struct ether_addr exterior_dev_mac;

uint16_t next_ext_port = 1024;


void memPrintHex(void* start, char* preamble, char* postamble, uint32_t len) {
	printf("%s", preamble);
	int i;
	for(i=0; i<len;i++) printf("%02x ", ((unsigned char*)start)[i]);
	printf("%s", postamble);
}

void fancyDumpState() {


	printf("/========START NAT TABLE========\\\n");
	int32_t iter = 0;
	nat_key_in2out* next_key;
	nat_value_in2out* next_value;
	char key_cl_ip_str[20];
	char key_srv_ip_str[20];
	char val_cl_ip_str[20];
	int64_t next_timeout;
	int32_t return_val;
	while (return_val = grt_redirect_iterate_snapshot(nat_table, (const void**)&next_key, (void**)&next_value, &next_timeout, &iter, 0) >= 0) {
		grt_ipv4_to_string(key_cl_ip_str, next_key->int_clIP);
		grt_ipv4_to_string(key_srv_ip_str, next_key->srvIP);
		grt_ipv4_to_string(val_cl_ip_str, next_value->ext_clIP);
		if (next_key->type == TYPE_NAT_KEY_IN2OUT) {
			printf(" (-> int_clIP=%s, srvIP=%s, int_clPort=%u, srvPort=%u, type=%u => ext_clIP=%s, ext_clPort=%u), return_val=%i, iter=%i.", 
			key_cl_ip_str, key_srv_ip_str, next_key->int_clPort, next_key->srvPort, next_key->type, val_cl_ip_str, next_value->ext_clPort,
			return_val, iter);
		} else {
			printf(" (<- ext_clIP=%s, srvIP=%s, ext_clPort=%u, srvPort=%u, type=%u => int_clIP=%s, int_clPort=%u), return_val=%i, iter=%i.", 
			key_cl_ip_str, key_srv_ip_str, next_key->int_clPort, next_key->srvPort, next_key->type, val_cl_ip_str, next_value->ext_clPort,
			return_val, iter);
		}
		memPrintHex(next_key, "key_hex: ", "\n", sizeof(nat_key_in2out)); 
	}
	printf("\\========END NAT TABLE========/ Final return value: %u\n", return_val);

	printf("/========START ARP TABLE========\\\n");
	iter = 0;
	arptable_key* next_key2;
	arptable_value* next_value2;
	char ip_str_buf[20];
	char ether_addr_string[20];
	while (return_val = grt_redirect_iterate_snapshot(arp_table, (const void**)&next_key2, (void**)&next_value2, &next_timeout, &iter, 0) >= 0) {
		grt_ipv4_to_string(ip_str_buf, next_key2->ip_addr);
		grt_ethaddr_to_string(ether_addr_string, &next_value2->mac_addr);
		printf(" (%s => %s), return_val=%i, iter=%i.\n", ip_str_buf, ether_addr_string, return_val, iter); 
	}
	printf("\\========END ARP TABLE========/ Final return value: %u\n", return_val);

	printf("/========START HOSTSTAT TABLE IN========\\\n");
	iter = 0;
	hsttable_key* next_key3;
	hsttable_value* next_value3;
	while (return_val = grt_redirect_iterate_snapshot(hst_table_in, (const void**)&next_key3, (void**)&next_value3, &next_timeout, &iter, 0) >= 0) {
		grt_ipv4_to_string(ip_str_buf, next_key3->int_clIP);
		printf(" (%s : %lu), return_val=%i, iter=%i.\n", ip_str_buf, next_value3->bytecount, return_val, iter); 
	}
	printf("\\========END HOSTSTAT TABLE IN========/ Final return value: %u\n", return_val);

	printf("/========START HOSTSTAT TABLE OUT========\\\n");
	iter = 0;
	hsttable_key* next_key4;
	hsttable_value* next_value4;
	while (return_val = grt_redirect_iterate_snapshot(hst_table_out, (const void**)&next_key4, (void**)&next_value4, &next_timeout, &iter, 0) >= 0) {
		grt_ipv4_to_string(ip_str_buf, next_key4->int_clIP);
		printf(" (%s : %lu), return_val=%i, iter=%i.\n", ip_str_buf, next_value4->bytecount, return_val, iter); 
	}
	printf("\\========END HOSTSTAT TABLE OUT========/ Final return value: %u\n", return_val);



}

//Returns > 0 if packet was forwarded, 0 if packet is OK but was not forwarded, and < 0 if error and not forwarded
int handle_arp(struct ether_hdr* l2hdr, uint16_t vlan_offset, struct rte_mbuf* packet, uint8_t grt_if_id, uint32_t my_ipv4, uint32_t my_maskv4, struct ether_addr* my_mac) {

	struct arp_hdr* arp_hdr = rte_pktmbuf_mtod_offset(packet, struct arp_hdr*, sizeof(struct ether_hdr) + vlan_offset);

	if (unlikely(__bswap_16(arp_hdr->arp_hrd) != ARP_HRD_ETHER)) {
		RTE_LOG(INFO, USER1, "    ARP header not of type Ethernet: %u, ignoring\n", arp_hdr->arp_hrd);
		return 0;
	}

	if (unlikely(__bswap_16(arp_hdr->arp_pro) != 0x0800)) {
		RTE_LOG(INFO, USER1, "    ARP header not for IP %u, ignoring.\n", arp_hdr->arp_pro);
		return 0;
	}

	if (unlikely(arp_hdr->arp_hln != 6)) {
		RTE_LOG(INFO, USER1, "    ARP hw size not 6, %u ignoring\n", arp_hdr->arp_hln);
		return 0;
	}	

	if (__bswap_16(arp_hdr->arp_op) == 1) {

		RTE_LOG(INFO, USER1, "    Is an ARP request.\n");

		struct arp_ipv4* arpdata = &arp_hdr->arp_data;

		RTE_LOG(INFO, USER1, "    Request is for IP address ");
		
		uint32_t swapped_tip = __bswap_32(arpdata->arp_tip);	//We have to swap the bytes again here to be conformant to DPDK representation
		print_ipv4(swapped_tip);
		printf("\n");

		if (unlikely(swapped_tip == my_ipv4)) {

			//printf("___");
			//print_ipv4(__bswap_32(arpdata.arp_sip)&interior_ipv4_mask);
			//printf("___");
			//print_ipv4(interior_ipv4_addr&interior_ipv4_mask);

			if (unlikely(__bswap_32(arpdata->arp_sip)&my_maskv4 != my_ipv4&my_maskv4)) {
				RTE_LOG(INFO, USER1, "    Request is for us, but from a wrong subnet...\n");
				return 0;
			}

			RTE_LOG(INFO, USER1, "    Request is for us. Send an arp reply\n");
			
			printf("a\n");

			memcpy(&l2hdr->d_addr, &arpdata->arp_sha, sizeof (struct ether_addr));	//Set dest MAC to arpdata source MAC

			struct grt_interface_info* if_info = grt_getInterfaceInfo(grt_if_id);

			if (is_macfilterfix_enabled()) {
				memcpy(&l2hdr->s_addr, &if_info->intf_mac, sizeof (struct ether_addr));
			} else {
				memcpy(&l2hdr->s_addr, my_mac, sizeof(struct ether_addr));	//Set source MAC to our mac
			}

			memcpy(&arpdata->arp_tha, &arpdata->arp_sha, sizeof(struct ether_addr));
			memcpy(&arpdata->arp_sha, my_mac, sizeof(struct ether_addr));
			arpdata->arp_tip = arpdata->arp_sip;
			arpdata->arp_sip = __bswap_32(my_ipv4);
			arp_hdr->arp_op = __bswap_16(2);

			if (grt_shall_output() <= 0) {
				return 0;
			}

			struct rte_mbuf* pkt2send_buf[1];
			pkt2send_buf[0] = packet;
	
			printf("Sending ARP on grt_if_id %u, %p, %p\n", grt_if_id, if_info, pkt2send_buf);

			int retrn = rte_eth_tx_burst(if_info->rte_port_id, 0, pkt2send_buf, 1);

			if (retrn == 1) {
				printf("    ARP was successfully sent.\n");
				return 1;
			} else {
				printf("    ARP was NOT successfully sent.\n");
				return -1;
			}
		}
	}

	if (__bswap_16(arp_hdr->arp_op) == 2) {
		//This is an ARP response for us (TODO: maybe check if it is really for us...)

		RTE_LOG(INFO, USER1, "    Is an ARP response.\n");

		struct arp_ipv4* arpdata = &arp_hdr->arp_data;

		if (unlikely(__bswap_32(arpdata->arp_sip)&my_maskv4 != my_ipv4&my_maskv4)) {
			RTE_LOG(INFO, USER1, "    Response is for us, but from a wrong subnet...\n");
			return 0;
		}

		arptable_key arpKey;
		arpKey.ip_addr = __bswap_32(arpdata->arp_sip);

		arptable_value* arpVal = (arptable_value*)rte_malloc("ARPTABLE_VALUE", sizeof(arptable_value), 0);
		memcpy(&arpVal->mac_addr, &arpdata->arp_sha, sizeof(struct ether_addr));

		int retrn = grt_redirect_table_put_notifystupd(arp_table, (void*)&arpKey, (void*)arpVal, -1, GRT_STUPD_TYPE_NEW_ARPENTRY);
		if (retrn >= 0) {
#ifdef grt_DEBUG
			char ip_str_buf[20];
			grt_ipv4_to_string(ip_str_buf, arpKey.ip_addr);
			char ether_addr_string[20]; 
			grt_ethaddr_to_string(ether_addr_string, &arpVal->mac_addr);
			RTE_LOG(INFO, USER1, "    Successfully stored ARP entry: (%s => %s)\n", ip_str_buf, ether_addr_string);
#endif
			return 0;	//The entry was successfully created.
		}
		RTE_LOG(INFO, USER1, "    Could not store ARP response in ARP table, table returned, %i\n", retrn);
		return -1;
	}

	//Unknown opcode, ignoring...
	return 0;

}

void _makeBroadcastMAC(struct ether_addr* addr) {
	unaligned_uint16_t* addr_words = (unaligned_uint16_t *)addr;
        addr_words[0] = 0xFFFF;
	addr_words[1] = 0xFFFF;
	addr_words[2] = 0xFFFF;
}

void _makeZeroMAC(struct ether_addr* addr) {

	unaligned_uint16_t* addr_words = (unaligned_uint16_t *)addr;
        addr_words[0] = 0;
	addr_words[1] = 0;
	addr_words[2] = 0;
}

int _doArpRequest(uint32_t ip2Seek, uint32_t myIP, struct ether_addr* myMac, uint8_t intf2Send) {
	struct grt_lcore_info_s* lc_info = grt_getCurrentLCoreInfo();
	struct rte_mbuf* m = rte_pktmbuf_alloc(lc_info->mempool);
	if (unlikely(m == NULL)) {
		printf(" ERROR: ARP request mbuf allocation failed.\n");
		return -1;
	}
	struct ether_hdr* eth_hdr = (struct ether_hdr*)rte_pktmbuf_append(m, sizeof(struct ether_hdr));
	if (unlikely(eth_hdr == NULL)) {
		printf(" ERROR: Could not allocate mbuf space for ether header.\n");
		return -1;
	}
	struct arp_hdr* arp_hdr = (struct arp_hdr*)rte_pktmbuf_append(m, sizeof(struct arp_hdr));
	if (unlikely(eth_hdr == NULL)) {
		printf(" ERROR: Could not allocate mbuf space for arp header.\n");
		return -1;
	}

	struct grt_interface_info* if_info = grt_getInterfaceInfo(intf2Send);

	_makeBroadcastMAC(&eth_hdr->d_addr);
	if (is_macfilterfix_enabled()) {
		memcpy(&eth_hdr->s_addr, &if_info->intf_mac, sizeof(struct ether_addr));
	} else {
		memcpy(&eth_hdr->s_addr, myMac, sizeof(struct ether_addr));
	}
	eth_hdr->ether_type = __bswap_16(ETHER_TYPE_ARP);

	arp_hdr->arp_hrd = __bswap_16(ARP_HRD_ETHER);
	arp_hdr->arp_pro = __bswap_16(0x0800);
	arp_hdr->arp_hln = 6;
	arp_hdr->arp_pln = 4;
	arp_hdr->arp_op = __bswap_16(ARP_OP_REQUEST);
	struct arp_ipv4* arpdata = &arp_hdr->arp_data;
	
	memcpy(&arpdata->arp_sha, myMac, sizeof(struct ether_addr));

	arpdata->arp_sip = __bswap_32(myIP);
	_makeZeroMAC(&arpdata->arp_tha);
	arpdata->arp_tip = __bswap_32(ip2Seek);

	if (grt_shall_output() <= 0) {
		return 0;
	}

	struct rte_mbuf* pkt2send_buf[1];

	pkt2send_buf[0] = m;
	int retrn = rte_eth_tx_burst(if_info->rte_port_id, 0, pkt2send_buf, 1);
	if (retrn == 1)  
		return 1;
	 else {
		//In an overload situation, the packets should be silently discarded here.
#ifdef ERRONEOUS_PACKET_FAILURES
		printf("    TX burst failed with error code %i.\n", retrn);
#endif
		return -1;
	}
	

}

int _createBidirectionalNatEntries(nat_value_in2out** resultEntry, nat_key_in2out* key, uint16_t ext_clPort, uint32_t ext_clIP) {
	//The next ext port for state updates is implicitly defined: ext_clPort+1

	*resultEntry = (nat_value_in2out*)rte_malloc("NAT_VALUE_IN2OUT", sizeof(nat_value_in2out), 0);
	(*resultEntry)->ext_clPort=ext_clPort;
	(*resultEntry)->ext_clIP=ext_clIP;

	//Create reverse key for new mapping...

	nat_key_out2in rev_key;
	rev_key.ext_clIP = exterior_ipv4_addr;
	rev_key.srvIP = key->srvIP;
	rev_key.ext_clPort = ext_clPort;
	rev_key.srvPort = key->srvPort;
	rev_key.type = TYPE_NAT_KEY_OUT2IN;

	//...and the reverse value...

	nat_value_out2in* rev_resultEntry = (nat_value_out2in*)rte_malloc("NAT_VALUE_OUT2IN", sizeof(nat_value_out2in), 0);
	rev_resultEntry->int_clPort=key->int_clPort;
	rev_resultEntry->int_clIP=key->int_clIP;

	int retrn = grt_redirect_table_put(nat_table, key, (*resultEntry), -1);
	if (retrn >= 0) {
		//The in->out entry was successfully created.
		int retrn2 = grt_redirect_table_put(nat_table, &rev_key, rev_resultEntry, -1);
		if (retrn2 >= 0) {
			return 1;	//The entry was not in the table but was successfully created.
		}
		printf(" ERROR in putting entry for out->in, grt_redirect_table_put(...) returned with %i\n", retrn);
		return -1;
	}

	//Error in putting
	printf(" ERROR in putting entry for in->out, grt_redirect_table_put(...) returned with %i\n", retrn);
	return -1;

}

int _getWrNatEntryFromIntPacket(nat_value_in2out** resultEntry, nat_key_in2out* key) {

	int retrn = grt_redirect_table_get(nat_table, key, (void**)resultEntry, 0);
	if (likely(retrn == 1)) return 0;	//The entry is in the table

	if (unlikely(retrn != -1)) {
		printf(" ERROR in getting, grt_redirect_table_get(...) returned with %i\n", retrn);
		//Error in getting
		return -1;
	}
	//Return is -1, so entry is not in the table.

	uint16_t ext_clPort = next_ext_port++;
	if (next_ext_port >= 65536) next_ext_port = MIN_EXT_PORT;
	uint32_t ext_clIP = exterior_ipv4_addr;

	if (grt_shall_notify_stupd()) {

		nat_stupd_entry* stupd = rte_malloc("STUP_DATA", sizeof(nat_stupd_entry), 0);
		stupd->int_clIP = key->int_clIP;
		stupd->ext_clIP = ext_clIP;
		stupd->srvIP = key->srvIP;
		stupd->int_clPort = key->int_clPort;
		stupd->ext_clPort = ext_clPort;
		stupd->srvPort = key->srvPort;

		grt_notify_stupd(GRT_STUPD_TYPE_NEW_NATENTRY, stupd, sizeof(stupd));
	}

	return _createBidirectionalNatEntries(resultEntry, key, ext_clPort, ext_clIP);

}


int _send_normal_ipv4(struct rte_mbuf* packet, struct ipv4_hdr* ip_hdr, struct udp_hdr* l4hdr, uint8_t l4proto, uint8_t intf2Send) {

	ip_hdr->time_to_live++;

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

	if (grt_shall_output() <= 0) {
		return 0;
	}

	struct grt_interface_info* if_info = grt_getInterfaceInfo(intf2Send);
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

void _countStat(uint32_t ip_src, uint64_t size2Add, grt_redirect_table* hst_tbl, uint16_t updateType) {

	hsttable_key key;
	key.int_clIP = ip_src;

	hsttable_value* foundEntry = NULL;

	int retrn = grt_redirect_table_get(hst_tbl, (void*)&key, (void**)&foundEntry, 0);

	uint64_t new_bytecount;
	if (unlikely(retrn != 1)) {	
		new_bytecount = size2Add;
	} else {
		new_bytecount = foundEntry->bytecount + size2Add;
	}

	hsttable_value* newEntry = rte_malloc("HSTTABLE_VALUE", sizeof(hsttable_value), 0);
		//In either case we need a new mem struct, do never overwrite the old one for snapshotting purposes!!!
	
	newEntry->bytecount = new_bytecount;

	retrn = grt_redirect_table_put_notifystupd(hst_tbl, (void*)&key, (void*)newEntry, -1, updateType);
	
	if (retrn < 0) {
		RTE_LOG(INFO, USER1, "    ERROR: Could not record host stats. \n");
	}
		

}


//Returns < 0 if packet was not relayed and must be freed.
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

	if (unlikely(next_proto != 6 && next_proto != 17)  ) {
		if (next_proto == 1) RTE_LOG(INFO, USER1, " ICMP NAT is currently not supported.\n");
		else RTE_LOG(INFO, USER1, " Cannot handle something different than TCP or UDP. Proto ID was: %u\n", next_proto);
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

	if (grt_if_id == 0) {

		//   /====== START Just for testing state updates

		//if (grt_shall_notify_stupd() > 0) {
		//	//RTE_LOG(INFO, USER1, "Sending state update vector intf0.\n");
		//	void* str = rte_malloc("STUP_DATA", 60, 0);
		//	int bytes = sprintf(str, "<Omitted during refactoring...>");
		//	grt_notify_stupd(123, str, 60);
		//}

		//   \====== END Just for testing state updates


		//Packet came from interior network.


		nat_key_in2out key;
		key.int_clIP = ip_src;
		key.srvIP = ip_dst;
		key.int_clPort = port_src;
		key.srvPort = port_dst;
		key.type = TYPE_NAT_KEY_IN2OUT;

		nat_value_in2out* lookupEntry = NULL;
		int retrn = _getWrNatEntryFromIntPacket(&lookupEntry, &key);

		if (retrn < 0) {
			RTE_LOG(INFO, USER1, "Error while getting NAT entry %i", retrn);
			return -1;
		}

#ifdef grt_DEBUG

		if (lookupEntry == NULL) {
			printf(" NAT entry was NULL, although correctly returned %i.\n", retrn);
			return -1;
		}

		printf(" NAT entry (int_clIP=%u, srvIP=%u, int_clPort=%hu, srvPort=%hu)->(ext_clIP=%u, ext_cl_Port=%hu)\n", 
			key.int_clIP, key.srvIP, key.int_clPort, key.srvPort, lookupEntry->ext_clIP, lookupEntry->ext_clPort);

#endif

		ip_hdr->src_addr = __bswap_32(lookupEntry->ext_clIP);
		udphdr->src_port = __bswap_16(lookupEntry->ext_clPort);

		//MAC table lookup...

		arptable_key arpKey;
		if (likely(ip_dst&exterior_ipv4_mask != exterior_ipv4_addr&exterior_ipv4_mask)) {
			//Not in our subnet, so use default gateway.
			arpKey.ip_addr = exterior_defaultgateway;
			//We could do a routing table lookup here in future versions.
		} else {
			//In our subnet, directly use its MAC.
			arpKey.ip_addr = ip_dst;
		}

		arptable_value* resultEntry;
#ifdef grt_DEBUG_DUMP_STATE
		fancyDumpState();
#endif

		retrn = grt_redirect_table_get(arp_table, (void*)&arpKey, (void**)&resultEntry, 0);

		if (unlikely(retrn != 1)) {
#ifdef grt_DEBUG
			char ip_str_buf[20];
			grt_ipv4_to_string(ip_str_buf, arpKey.ip_addr);
			RTE_LOG(INFO, USER1, "  Did not find an ARP entry for IP address %s. Making an ARP request.\n", ip_str_buf);
#endif
			if (unlikely(retrn != -1)) {
				//Unknown error occured.
				printf("    Error occurred when trying to get entry from ARP table, retrn=%i.\n", retrn);
			}
			//Entry was not in the table, so send arp.
			_doArpRequest(arpKey.ip_addr, exterior_ipv4_addr, &exterior_dev_mac, 1);

			return 0;
		}

#ifdef grt_DEBUG
		char ip_str_buf[20];
		grt_ipv4_to_string(ip_str_buf, arpKey.ip_addr);
		char ether_addr_string[20]; 
		grt_ethaddr_to_string(ether_addr_string, &resultEntry->mac_addr);
		RTE_LOG(INFO, USER1, "  Found an ARP entry for IP address %s (%s). Forwarding packet...\n", ip_str_buf, ether_addr_string);
#endif

		_countStat(ip_src, packet->pkt_len, hst_table_out, GRT_STUPD_TYPE_HST_UPDATE_OUT);

		memcpy(&eth_hdr->d_addr, &resultEntry->mac_addr, sizeof (struct ether_addr));


		struct grt_interface_info* if_info = grt_getInterfaceInfo(1);

		if (is_macfilterfix_enabled()) {
			memcpy(&eth_hdr->s_addr, &if_info->intf_mac, sizeof (struct ether_addr));
		} else {
			memcpy(&eth_hdr->s_addr, &exterior_dev_mac, sizeof (struct ether_addr));
		}

		return _send_normal_ipv4(packet, ip_hdr, udphdr, next_proto, 1);

	}

	if (grt_if_id == 1) {

		//   /====== START Just for testing state updates

		//if (grt_shall_notify_stupd() > 0) {
		//	//RTE_LOG(INFO, USER1, "Sending state update vector intf1.\n");
		//	void* str = rte_malloc("STUP_DATA", 50, 0);
		//	int bytes = sprintf(str, "Happy State Migration!");
		//	grt_notify_stupd(122, str, 50);
		//}

		//   \====== END Just for testing state updates

		//Packet came from exterior network.


		nat_key_out2in key;
		key.ext_clIP = ip_dst;
		key.srvIP = ip_src;
		key.ext_clPort = port_dst;
		key.srvPort = port_src;
		key.type = TYPE_NAT_KEY_OUT2IN;


#ifdef grt_DEBUG_DUMP_STATE
		fancyDumpState();
		printf(" Looking for NAT key (< ext_clIP=%u, srvIP=%u, ext_clPort=%hu, srvPort=%hu)", 
			key.ext_clIP, key.srvIP, key.ext_clPort, key.srvPort);
#endif


		//memPrintHex(&key, "key_hex: ", "\n", sizeof(nat_key_in2out));

		nat_value_out2in* lookupEntry = NULL;
		int retrn = grt_redirect_table_get(nat_table, &key, (void**)&lookupEntry, 0);


		if (unlikely(retrn == -1)) {
			//Not in table. Drop packet.
			RTE_LOG(INFO, USER1, " no assoc. Was looking for NAT key (< ext_clIP=%u, srvIP=%u, ext_clPort=%hu, srvPort=%hu, frag_offset=%hu, packet_id=%hu)\n", 
			key.ext_clIP, key.srvIP, key.ext_clPort, key.srvPort, __bswap_16(ip_hdr->fragment_offset), __bswap_16(ip_hdr->packet_id));
			return 0;
		}
		if (unlikely(retrn != 1)) {
			RTE_LOG(INFO, USER1, " ERROR while querying NAT table, return code %i.\n", retrn);
			return -1;
		}

#ifdef grt_DEBUG

		if (lookupEntry == NULL) {
			printf(" NAT entry was NULL, although correctly returned %i.\n", retrn);
			return -1;
		}

		printf(" NAT entry (< ext_clIP=%u, srvIP=%u, ext_clPort=%hu, srvPort=%hu)->(int_clIP=%u, int_clPort=%hu)\n", 
			key.ext_clIP, key.srvIP, key.ext_clPort, key.srvPort, lookupEntry->int_clIP, lookupEntry->int_clPort);

#endif

		ip_dst = lookupEntry->int_clIP;
		port_dst = lookupEntry->int_clPort;

		_countStat(ip_dst, packet->pkt_len, hst_table_in, GRT_STUPD_TYPE_HST_UPDATE_IN);

		ip_hdr->dst_addr = __bswap_32(ip_dst);
		udphdr->dst_port = __bswap_16(port_dst);

		//MAC table lookup...

		arptable_key arpKey;
		if (likely(ip_dst&exterior_ipv4_mask != interior_ipv4_addr&interior_ipv4_mask)) {
			//Not in our subnet, so use default gateway.
			//arpKey.ip_addr = interior_defaultgateway;
			//We could do a routing table lookup here in future versions.
			printf(" Dropping NATted exterior packet, as nested subnets currently unsupported.\n");
			return -1;
		} else {
			//In our subnet, directly use its MAC.
			arpKey.ip_addr = ip_dst;
		}

		arptable_value* resultEntry;

		retrn = grt_redirect_table_get(arp_table, (void*)&arpKey, (void**)&resultEntry, 0);

		if (unlikely(retrn != 1)) {
#ifdef grt_DEBUG
			char ip_str_buf[20];
			grt_ipv4_to_string(ip_str_buf, arpKey.ip_addr);
			RTE_LOG(INFO, USER1, "  Did not find an ARP entry for IP address %s. Making an ARP request.\n", ip_str_buf);
#endif
			if (unlikely(retrn != -1)) {
				//Unknown error occured.
				printf("    Error occurred when trying to get entry from ARP table, retrn=%i.\n", retrn);
			}
			//Entry was not in the table, so send arp.
			_doArpRequest(arpKey.ip_addr, interior_ipv4_addr, &interior_dev_mac, 0);

			return 0;
		}

#ifdef grt_DEBUG
		char ip_str_buf[20];
		grt_ipv4_to_string(ip_str_buf, arpKey.ip_addr);
		char ether_addr_string[20]; 
		grt_ethaddr_to_string(ether_addr_string, &resultEntry->mac_addr);
		RTE_LOG(INFO, USER1, "  Found an ARP entry for IP address %s (%s). Forwarding packet...\n", ip_str_buf, ether_addr_string);
#endif

		memcpy(&eth_hdr->d_addr, &resultEntry->mac_addr, sizeof (struct ether_addr));

		
		struct grt_interface_info* if_info = grt_getInterfaceInfo(0);

		if (is_macfilterfix_enabled()) {
			memcpy(&eth_hdr->s_addr, &if_info->intf_mac, sizeof (struct ether_addr));
		} else {
			memcpy(&eth_hdr->s_addr, &interior_dev_mac, sizeof (struct ether_addr));
		}

		return _send_normal_ipv4(packet, ip_hdr, udphdr, next_proto, 0);

	}

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
		if (handle_ipv4(0, packet, grt_if_id) <= 0) {
			rte_pktmbuf_free(packet);
		};
		return 0;

	} else if (l2hdr->ether_type == 0x0608) {
		//We have an ARP packet. Note that the two bytes have to be swapped.
		RTE_LOG(INFO, USER1, "  ARP packet received.\n");
		if (grt_if_id==0) {

			//Interior interface
			if (handle_arp(l2hdr, 0, packet, grt_if_id, interior_ipv4_addr, interior_ipv4_mask, &interior_dev_mac) <= 0) {
				rte_pktmbuf_free(packet);
			}
			return 0;
		} else {
			//Exterior interface
			if (handle_arp(l2hdr, 0, packet, grt_if_id, exterior_ipv4_addr, exterior_ipv4_mask, &exterior_dev_mac) <= 0) {
				rte_pktmbuf_free(packet);
			}
			return 0;
		}

	} else if (l2hdr->ether_type == 0x0081) {
		RTE_LOG(INFO, USER1, "  VLAN-tagged frame received.\n");
	}

	return 0;

}

int handle_snapshot_out() {

	grt_redirect_set_snap_state(nat_table, GRT_RT_S_SNAPSHOTTING);
	grt_redirect_set_snap_state(arp_table, GRT_RT_S_SNAPSHOTTING);
	grt_redirect_set_snap_state(hst_table_in, GRT_RT_S_SNAPSHOTTING);
	grt_redirect_set_snap_state(hst_table_out, GRT_RT_S_SNAPSHOTTING);

	void* snapBuf = rte_zmalloc("SNAPSHOT_OUT_BUFFER", SNAPSHOT_BUF_SIZE, 0);
	void* snapBufOffsetMax = snapBuf + SNAPSHOT_BUF_SIZE;
	void* snapBufOffset = snapBuf;

#ifdef grt_DEBUG_SNAPSHOTS
	void* offsetBefore = snapBufOffset;
#endif
	int retrn = grt_redirect_serialize_snapshot(nat_table, &snapBufOffset, snapBufOffsetMax);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Serialized NAT table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, snapBufOffset, snapBufOffset-offsetBefore, retrn);
	offsetBefore = snapBufOffset;
#endif
	retrn = grt_redirect_serialize_snapshot(arp_table, &snapBufOffset, snapBufOffsetMax);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Serialized ARP table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, snapBufOffset, snapBufOffset-offsetBefore, retrn);
	offsetBefore = snapBufOffset;
#endif
	grt_redirect_serialize_snapshot(hst_table_in, &snapBufOffset, snapBufOffsetMax);
	retrn = grt_redirect_serialize_snapshot(hst_table_out, &snapBufOffset, snapBufOffsetMax);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Serialized Hoststate tables. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
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

	grt_redirect_set_snap_state(nat_table, GRT_RT_S_IDLE);
	grt_redirect_set_snap_state(arp_table, GRT_RT_S_IDLE);
	grt_redirect_set_snap_state(hst_table_in, GRT_RT_S_IDLE);
	grt_redirect_set_snap_state(hst_table_out, GRT_RT_S_IDLE);

	return 0;

}

//This is guranteed not to interrupt the snapshot install processs.
int handle_state_update(uint16_t type, void* stupd_vec, uint16_t len) {

	if (type == GRT_STUPD_TYPE_NEW_ARPENTRY) {

		grt_redirect_table_put_fromstupd(arp_table, stupd_vec);
		return 1;
	}

	if (type == GRT_STUPD_TYPE_HST_UPDATE_IN || type == GRT_STUPD_TYPE_HST_UPDATE_OUT) {

		grt_redirect_table* thetable = (type == GRT_STUPD_TYPE_HST_UPDATE_IN) ? hst_table_in : hst_table_out;

#ifdef DEBUG_HST_SEQUENCE

		//printf("Received state update vector of type %u, len %u", type, len);
		//grt_printHexArray(stupd_vec, len);
		//printf("\n");

		hsttable_key* upd_key = (hsttable_key*)stupd_vec;
		hsttable_value* upd_val = (hsttable_value*)(stupd_vec + sizeof(hsttable_key));

		hsttable_value* foundEntry = NULL;
		char ip_str_buf[20];
		grt_ipv4_to_string(ip_str_buf, upd_key->int_clIP);
		int retrn = grt_redirect_table_get(thetable, (void*)upd_key, (void**)&foundEntry, 0);
		if (unlikely(retrn >= 0)) {
			if (foundEntry->bytecount >= upd_val->bytecount) {
				printf("STUPD: We have an entry for %s, updated value is  %lu, original value is %lu. OUT of ORDER!\n", ip_str_buf, upd_val->bytecount, foundEntry->bytecount);
			} else {
				printf("STUPD: We have an entry for %s, updated value is  %lu, original value is %lu, diff=%lu In order.\n", ip_str_buf, upd_val->bytecount, foundEntry->bytecount, upd_val->bytecount-foundEntry->bytecount);
			}
		} else {
			printf("STUPD: We do not have an entry for %s, updated value is  %lu\n", ip_str_buf, upd_val->bytecount);
		}
		

#endif

		grt_redirect_table_put_fromstupd(thetable, stupd_vec);
		return 1;
	}

	if (type == GRT_STUPD_TYPE_NEW_NATENTRY) {

		nat_stupd_entry* natentry = (nat_stupd_entry*)stupd_vec;
		
		nat_key_in2out natkey;
		natkey.int_clIP = natentry->int_clIP;
		natkey.srvIP = natentry->srvIP;
		natkey.int_clPort = natentry->int_clPort;
		natkey.srvPort = natentry->srvPort;

		nat_value_in2out* dummyEntry;	//we don't  need a result... But don't free it, used by the table!
		_createBidirectionalNatEntries(&dummyEntry, &natkey, natentry->ext_clPort, natentry->ext_clIP);
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
	int retrn = grt_redirect_deserialize_snapshot(nat_table, &currentOffset);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Deserialized NAT table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, currentOffset, currentOffset-offsetBefore, retrn);
	offsetBefore = currentOffset;
#endif
	retrn = grt_redirect_deserialize_snapshot(arp_table, &currentOffset);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Deserialized ARP table. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
		offsetBefore, currentOffset, currentOffset-offsetBefore, retrn);
	offsetBefore = currentOffset;
#endif
	grt_redirect_deserialize_snapshot(hst_table_in, &currentOffset);
	retrn = grt_redirect_deserialize_snapshot(hst_table_out, &currentOffset);
#ifdef grt_DEBUG_SNAPSHOTS
	RTE_LOG(INFO, USER1, "Deserialized Hoststate tables. old offset=%p, new offset=%p, diff=%li, return code=%i\n", 
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
	interior_ipv4_addr = IPv4(192,168,0,254);
	interior_ipv4_mask = IPv4(255,255,255,0);
	
	exterior_ipv4_addr = IPv4(192,168,1,254);
	exterior_ipv4_mask = IPv4(255,255,255,0);

	exterior_defaultgateway = IPv4(192,168,1,1);

	const struct ether_addr intAddr2set = {.addr_bytes={0x52,0x54,0x00,0x00,0x00,0x01}};	//52:54:00:00:00:01
	memcpy(&interior_dev_mac, &intAddr2set, sizeof(struct ether_addr));

	const struct ether_addr extAddr2set = {.addr_bytes={0x52,0x54,0x00,0x00,0x00,0x02}};	//52:54:00:00:00:02
	memcpy(&exterior_dev_mac, &extAddr2set, sizeof(struct ether_addr));

	//if LOG
	char ethaddr_str_buf [20];
	grt_ethaddr_to_string(ethaddr_str_buf, &interior_dev_mac);
	printf("Interior MAC: %s\n", ethaddr_str_buf);

	printf("Interior IP address: ");
	print_ipv4(interior_ipv4_addr);
	printf("\n");
	//endif LOG
}

int prepareInEAL() {
	static struct rte_hash_parameters defaultParamsNat = {
		.name = "GRT_NAT_TABLE",
		.entries = 65536,
		.reserved = 0,
		.key_len = sizeof(nat_key_in2out),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
		.socket_id = 0,
		.extra_flag = 0,
	};
	static struct rte_hash_parameters defaultParamsArp = {
		.name = "GRT_ARP_TABLE",
		.entries = 65536,
		.reserved = 0,
		.key_len = sizeof(arptable_key),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
		.socket_id = 0,
		.extra_flag = 0,
	};

	static struct rte_hash_parameters defaultParamsHoststatIn = {
		.name = "GRT_HST_TABLE_IN",
		.entries = 65536,
		.reserved = 0,
		.key_len = sizeof(hsttable_key),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
		.socket_id = 0,
		.extra_flag = 0,
	};

	static struct rte_hash_parameters defaultParamsHoststatOut = {
		.name = "GRT_HST_TABLE_OUT",
		.entries = 65536,
		.reserved = 0,
		.key_len = sizeof(hsttable_key),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
		.socket_id = 0,
		.extra_flag = 0,
	};


	printf("[App] Creating NAT, ARP and Hoststat table...\n");

	printf("[App] sizeof(nat_key_in2out)=%lu (should be 13 if optimally compressed).\n", sizeof(nat_key_in2out));

	nat_table = grt_redirect_table_create(&defaultParamsNat, sizeof(nat_key_in2out), sizeof(nat_value_in2out));
	arp_table = grt_redirect_table_create(&defaultParamsArp, sizeof(arptable_key), sizeof(arptable_value));
	hst_table_in = grt_redirect_table_create(&defaultParamsHoststatIn, sizeof(hsttable_key), sizeof(hsttable_value));
	hst_table_out = grt_redirect_table_create(&defaultParamsHoststatOut, sizeof(hsttable_key), sizeof(hsttable_value));

	printf("[App] Created NAT, ARP and Hoststat table.\n");

	return 23;

}


int main(int argc, char **argv) {

	prepare();

	printf("Starting NAT app for SliM...\n");
	grt_main(argc, argv, &prepareInEAL, &handle_packet, &handle_snapshot_out, &handle_snapshot_in, &handle_state_update);

}

