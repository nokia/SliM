#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_errno.h>

#define GRT_BURSTLEN	4

#define GRT_VLAN_CTRL_ID 	78
#define GRT_VLAN_P2P_ID 	79

#define WAIT_RING_SIZE		16384*4

//#define grt_DEBUG


int handle_replicated_packet(uint16_t type, void* data, uint16_t len) {

	uint8_t rte_port_id = *((uint8_t*)data);
	//TODO: enable again...
	//printf("Handling duplicated packet of type %u, length %u, rte_port_id=%u", type, len, rte_port_id);
	struct rte_mbuf* pktclone = rte_pktmbuf_alloc(grt_replication_mempool);
	void* pktdata = rte_pktmbuf_append(pktclone, len - sizeof(uint8_t));
	rte_memcpy(pktdata, data+sizeof(uint8_t), len - sizeof(uint8_t));
	struct ether_hdr* l2hdr;
	l2hdr = rte_pktmbuf_mtod(pktclone, struct ether_hdr*);

	(*grt_handlePktFunc)(l2hdr, pktclone, rte_port_id);
	rte_free(data);
	return 0;
}

void handle_packet_firststage(struct rte_mbuf* pkt, uint8_t lcore_id, uint8_t grt_core_index, uint8_t rte_port_id) {

	//printf ("Called handle_packet with %p, %u, %u, %u)\n", pkt, lcore_id, grt_core_index, rte_port_id);

	rte_prefetch0(rte_pktmbuf_mtod(pkt, void*));



	struct ether_hdr* l2hdr;			
	l2hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr*);

	if (unlikely(grt_dataplane_state < 2)) {
		RTE_LOG(INFO, USER1, "Packet received with ethtype 0x%04x on interface %u, although nothing should come in (dataplane state = %u). Unclean SDN?\n", 
			__bswap_16(l2hdr->ether_type), rte_port_id, grt_dataplane_state);
		rte_pktmbuf_free(pkt);
		return;
	}

#ifdef grt_DEBUG
	RTE_LOG(INFO, USER1, "=> Packet received... lcore_id=%u, grt_core_index=%u, rte_port_id=%u, ethtype=%u\n", lcore_id, grt_core_index, rte_port_id, l2hdr-> ether_type);
#endif
	if (unlikely(l2hdr->ether_type == 0x9a08)) {		//swapped for LE
		//Drain packet received.
		onDrainPacketReceived(grt_core_index);				
			//FIXME: Switching to AWAITING_STATE could be a little delayed here.

		rte_pktmbuf_free(pkt);
		return;
	}

	if (unlikely(grt_shall_notify_stupd_legacy() > 0)) {
		void* packetDataPtr = rte_pktmbuf_mtod(pkt, void*);
		uint16_t packetsz = pkt->data_len;
		//printf("Duplicating whole packet with size %u.", packetsz);
		uint8_t* pktclone = rte_malloc("DUPLICATED_PACKET", packetsz + sizeof(uint8_t), 0);
		*((uint8_t*)pktclone) = rte_port_id;	//we need to know which interface it came from, so first byte is the intf id.
		rte_memcpy(pktclone+sizeof(uint8_t), packetDataPtr, packetsz); //MUST be memcpied, the original may change in the processing pipe!
		grt_notify_stupd_legacy(GRT_STUPD_TYPE_WHOLEPACKET, pktclone, packetsz + sizeof(uint8_t));
	}

	(*grt_handlePktFunc)(l2hdr, pkt, grt_core_index);	//Here the function of the grt app is called to handle the "userland" packet.
	
}

void clearWaitQueue(uint8_t lcore_id, uint8_t grt_core_index, uint8_t rte_port_id) {
	int count = rte_ring_count(grt_lcore_info[grt_core_index]->waitbuf);
	int status;
	if (likely(count <= 0)) return;
	//printf ("Called clearWaitQueue with %u, %u, %u, packets: %u)\n", lcore_id, grt_core_index, rte_port_id, count);
	printf("waitq_handled_packets_%i=%i\n", grt_core_index, count);

	struct rte_mbuf* queuedPkt;
	while (1) {		
		status = rte_ring_sc_dequeue(grt_lcore_info[grt_core_index]->waitbuf, (void**)&queuedPkt);
		//printf ("Dequeued %p, %i", queuedPkt, status);
		if (likely(status != 0)) break;
		//printf("deq: %u\n", rte_ring_count(waitq));
		handle_packet_firststage(queuedPkt, lcore_id, grt_core_index, rte_port_id);
	}

}

static int do_dataplane_job(__attribute__((unused)) void *dummy) {

	uint8_t lcore_id = rte_lcore_id();

	RTE_LOG(INFO, USER1, "Called do-dataplane-job, lcore=%i\n", lcore_id);

	uint8_t grt_core_index = lcore_to_available_idx[lcore_id];	//Is equal to the GRT interface number!
	uint8_t rte_port_id = devs_port_ids[grt_core_index];

	if (!rte_lcore_is_enabled(lcore_id) || rte_get_master_lcore() == lcore_id) {
		RTE_LOG(INFO, USER1, "Lcore %u is not enabled or master core. It has nothing to do here. Exiting thread.\n", lcore_id);
		return 1;
	}

	if (grt_core_index >= num_devs) {
		//We only let lcores with an interface do something here.
		RTE_LOG(INFO, USER1, "Lcore %u is not assigned to an interface, it has nothing to do, exiting in this thread.\n", lcore_id);
		return 0;
	}




	struct rte_mbuf* rcv_pkt_bucket[GRT_BURSTLEN];

	//printf("rte_port_id=%u, grt_core_index=%u, lcore_id=%u", rte_port_id, grt_core_index, lcore_id);

	//int bursts=0;
	grt_lcore_info[grt_core_index]->debug = 0;

	//Remember lcore[i] is responsible for the respective interface.
	while(1) {

		uint32_t rx_pkt_count = rte_eth_rx_burst(rte_port_id, 0, rcv_pkt_bucket, GRT_BURSTLEN);

		/*
		if (bursts >= 100000) {
			printf("...%u... dpstate=%i \n", grt_core_index, grt_dataplane_state);
			bursts = 0;
		}
		bursts += rx_pkt_count;
		*/

		int i;
		if (rx_pkt_count == 0 && grt_dataplane_state >= 2) clearWaitQueue(lcore_id, grt_core_index, rte_port_id);

		for (i=0; i < rx_pkt_count; i++) {
			int status;

			//printf("A %u\n", rte_port_id);

			if (unlikely(grt_dataplane_state == 1)) {
				
				//rte_pktmbuf_free(rcv_pkt_bucket[i]);

	
				status = rte_ring_sp_enqueue(grt_lcore_info[grt_core_index]->waitbuf, rcv_pkt_bucket[i]);
				//printf("enq: %u\n", rte_ring_count(waitbuf));
				if (status == -ENOBUFS) {
					overflown_pkt_q++;
					rte_pktmbuf_free(rcv_pkt_bucket[i]);
				}

				continue;
			}

			if (grt_dataplane_state >=2) clearWaitQueue(lcore_id, grt_core_index, rte_port_id);
			
			//printf("B %u\n", rte_port_id);

			handle_packet_firststage(rcv_pkt_bucket[i], lcore_id, grt_core_index, rte_port_id);


		}
	}
	
	return 0;


}


