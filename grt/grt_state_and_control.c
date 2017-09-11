
#include <netinet/in.h>
#include <byteswap.h>
#include <semaphore.h>
#include <arpa/inet.h>


#define STUP_SEND_RING_SIZE		65536
#define STUP_RECV_RING_SIZE		65536*8	//Must likely be larger at the end!
#define STUP_RECV_FINISHED_THRESH 	0

#define DEBUG_STATELET_QUEUE_QUANT	5000

#define TCP_PORT_CTRL 	1400
#define TCP_PORT_SNAP	1401
#define TCP_PORT_STUP 	1402



//============== CTRL_CMDS ===================

#define CTRL_MSG_POKE			10	//6 padding bits	Is sent to the client from ctrl to test connection
#define CTRL_MSG_STATE_TRANSFER 	11	//2 padding bits + 4bytes destination IP	Is sent to the VM from the server.
#define CTRL_MSG_STUPD_OK		12	//6 padding bits	Is sent to the controller from the client.
#define CTRL_MSG_INIT			13	//6 padding bits	Is sent to the client from ctrl to clear and start fwding (puts it to WORKING state).



//============== STATES ===================

#define GRT_S_WORKING		0
//The machine is currently forwarding normally.

//If the controller sends CTRL_MSG_STATE_TRANSFER(dest), set the machine to GRT_S_UPD_PROV(dest).

#define GRT_S_AWAITING_STATE	1	
/*
Optional initial state. 

The machine is currently awaiting the state transfer.
It waits for incoming connections on port TCP_PORT_SNAP (snap transfer), and renders it as a stream to the application.
It waits for incoming connections on port TCP_PORT_STUP (state update transfer), and fully buffers the incoming stream.
Nothing is forwarded, everything is dropped.

After full reception and installation of the state (SNAP closed), switch to GRT_S_UPD_CONS.

Throw an error (and fall back to GRT_S_WORKING if operational), if any buffer runs over.
*/

#define GRT_S_UPD_PROV		2	
/*
Triggered by the controller via CTRL_MSG_STATE_TRANSFER(dest)

- Opens TCP streams TCP_PORT_SNAP and TCP_PORT_STUP to dest. Commands the application to produce, and sends the obtained 
snapshot to dest via TCP_PORT_SNAP.
- If the application produces state update vectors, their stream is sent out 
via TCP_PORT_STUP to dest.

When a drain packet was received on every input port, close connection, delete state, and go into GRT_S_AWAITING_STATE mode.

*/

#define GRT_S_UPD_CONS		3
/*
Entered after GRT_S_AWAITING_STATE, after state was fully transferred (TCP_PORT_SNAP closed).

It waits for incoming connections on port TCP_PORT_STUP (state update transfer), buffers the incoming stream.
The buffer is drained, the state update packets are entered sequentially into the transferred and installed state.
No packets are outputted

After the state update buffer enters a certain threshold, asynchronously send CTRL_MSG_STUPD_OK to controller.

If TCP_PORT_STUP connection is closed, enter GRT_S_WORKING

*/

//=========== PRE-DEFS =============

//void log_state(char*); //deprecated

void init_s_working();

int handle_replicated_packet(uint16_t type, void* data, uint16_t len);	//is in grt_dataplane.c

//=========== END PRE-DEFS =============

pthread_t ctrl_thread;
pthread_t snap_thread;
pthread_t stup_tcping_thread;

pthread_t snap_rcv_thread;
pthread_t stup_rcv_thread;

int sockfd_ctrl;
struct sockaddr_in serv_addr = { .sin_family=AF_INET, };

uint8_t grt_state = GRT_S_AWAITING_STATE;
uint8_t grt_dataplane_state = 0;	//0: Drop packet, 1: Buffer, do not process yet, 2: Process packets (drain previously buffered)
int shall_send_stupd = 0;		//1 if state updates shall be sent, 0 or -1 otherwise. -1 means, the dequeue thread can also be killed and the connection closed.

uint64_t start_time_cycles;	//Used to track the duration of the state transfer.

/*
 Only open and valid during snapshot transfer
*/
int sockfd_snap = -1;
int sockfd_snap_rcv = -1;
int sockfd_snap_rcv_clt = -1;

/*
 Only open and valid during state update transfer
*/
int sockfd_stup = -1;
int sockfd_stup_rcv = -1;
struct sockaddr_in stup_addr = { .sin_family=AF_INET, };

struct rte_ring* stup_send_ring;
#ifdef SEMAPHORE
sem_t stup_send_ring_deq_sem;
#endif

/*
 Only open and valid during state update reception
*/
struct rte_ring* stup_recv_ring;
#ifdef SEMAPHORE
sem_t stup_recv_ring_deq_sem;
#endif

//For stats, to get the number of dropped packets on pkt_q. Written in grt_dataplane.c
uint64_t overflown_pkt_q = 0;



uint32_t dest_ip_buf;	//Just used to carry it to the thread.

typedef	struct {
		uint16_t opcode;
		uint16_t unused;
		uint32_t addr;
} ctrl_msg;

typedef struct  __attribute__((__packed__)) {
		uint16_t type;
		uint16_t len;
		void*	data;		
} stup_vec;


int send_ctrl_msg(ctrl_msg* msg2send) {

	int n = send(sockfd_ctrl, (char*)msg2send, 8, 0);
	if (n < 0)
    		rte_exit(EXIT_FAILURE, "ERROR writing to socket");
		return -2;
	if (n != 8) {
		printf("  [S] ERROR: Could not send whole ctrl msg, only %i bytes", n);
		return -1;
	}
	return 0;

}

int send_stupd_ok_async() {
	ctrl_msg buf;
	buf.opcode = __bswap_16(CTRL_MSG_STUPD_OK);
	send_ctrl_msg(&buf);
}

#define DRAIN_PKTS_NO 2
int drainPktsReceived = 0;

void onDrainPacketReceived(uint8_t dp_index) {
	// TODO: Currently does not cope with packet loss or retransmits.

	drainPktsReceived++;

	if (drainPktsReceived < DRAIN_PKTS_NO) {
		printf("  [S] INFO: Drain packet received on datapath index %u, still not enough: %i.\n", dp_index, drainPktsReceived);
		return;
	}

	if (shall_send_stupd > 0) {
		grt_dataplane_state = 0;
		printf("  [S] INFO: Last drain packet received on datapath index %u.\n", dp_index);
		shall_send_stupd = -1;
#ifdef SEMAPHORE
		sem_post(&stup_send_ring_deq_sem);	//Poke the dequeue thread as if something was enqueued.
#endif
	} else {
		printf("  [S] INFO: Drain packet received on other datapath, index %u, ignoring.\n", dp_index);
			//FIXME: As the dataplane closes on first drain packet coming in, this does not work. Maybe that must be fixed with multi-datapath closing.
	}
}


//ALSO handles received stup receive queue DEqueues, after snapshot installation.
void* handle_snapshot_receive(void* nullval) {

	struct sockaddr_in snap_addr = {
		.sin_family=AF_INET,
		.sin_addr={
			.s_addr=0 //0.0.0.0 will listen on all intfs (NOT secure code!). 
		},			
	};
	int status;
	struct sockaddr_in snap_rcv_clnt_addr;
	unsigned int snap_rcv_clnt_addr_len;

	snap_addr.sin_port=__bswap_16(TCP_PORT_SNAP);

	sockfd_snap_rcv = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_snap_rcv < 0)
		rte_exit(EXIT_FAILURE, "Error opening socket for snapshot traffic. '%i'.\n", sockfd_snap_rcv); //TODO: maybe no complete halt here...

	status = bind(sockfd_snap_rcv, (struct sockaddr*)&snap_addr, sizeof(struct sockaddr_in));
	if (status < 0) rte_exit(EXIT_FAILURE, "Error binding socket for snapshot traffic. '%i'.\n", status); //TODO: maybe no complete halt here...

	status = listen(sockfd_snap_rcv, 1);	//only 1 connection allowed.
	if (status < 0) rte_exit(EXIT_FAILURE, "Error listening to incoming snapshot transfers. '%i'.\n", status); //TODO: maybe no complete halt here...
	
	snap_rcv_clnt_addr_len = sizeof(struct sockaddr_in);	

	sockfd_snap_rcv_clt = accept(sockfd_snap_rcv, (struct sockaddr*)&snap_rcv_clnt_addr, &snap_rcv_clnt_addr_len);
	if (sockfd_snap_rcv_clt < 0) {
		if (errno == EINVAL) {
			printf("   [Snapshotting] The waiting for snapshots was interrupted...\n");
			close(sockfd_snap_rcv_clt);
			close(sockfd_snap_rcv);
			sockfd_snap_rcv = -1;
			return 0;
		} 
		else rte_exit(EXIT_FAILURE, "Error while accepting incoming connections for snapshot transfer. '%i'.\n", errno); //TODO: maybe no complete halt here...
	}

	grt_state = GRT_S_UPD_CONS;
	printf(" [S] Starting snapshot transfer from here.");
	start_time_cycles = rte_get_timer_cycles();


	int retrn = (*grt_handleSnInFunc)();
	if (retrn < 0)
		rte_exit(EXIT_FAILURE, "VNF notified error while receiving the snapshot function."); //TODO: maybe no complete halt here...

	printf("   [Snapshotting] Snapshot fully received and installed.\n");

	close(sockfd_snap_rcv_clt);
	sockfd_snap_rcv_clt = -1;
	close(sockfd_snap_rcv);
	sockfd_snap_rcv = -1;

	printf("   [StateUpdating] Draining queue of received state updates now.\n");

	stup_vec* vec2Deq;

	int debugRepeat = 0;
	int stupd_ok_sent = 0;	// > 0, if state update OK already sent to the controller.

	while(1) {

		uint32_t count = rte_ring_count(stup_recv_ring);		//TODO: use this for stats and eval.
		if (stupd_ok_sent <= 0 && count <= STUP_RECV_FINISHED_THRESH) {
			printf("   [StateUpdating] Threshold underrun, notifying controller that it is okay to switch over: %u\n", count);
			grt_dataplane_state = 1; // Buffer incoming packets. as from here on, switchover may happen suddenly!
			drainPktsReceived = 0; //From here, drain packets could come...
			send_stupd_ok_async();
			stupd_ok_sent = 1;
		}

		int status = rte_ring_sc_dequeue(stup_recv_ring, (void**)&vec2Deq);
		if (status != 0) {
			if (sockfd_stup_rcv == -1) {
				//This means connection from sender was closed, and as we have 0 packets, our updating is finished.
				printf("   [StateUpdating] Sender has quit state updating. We are now switched over. \n");
				break;
			}
#ifdef SEMAPHORE
			sem_wait(&stup_recv_ring_deq_sem);	//Blocks until something has been enqueued
#endif
			continue;
		}
		
		if (debugRepeat >= DEBUG_STATELET_QUEUE_QUANT) {
			printf("Current stup packets in receive ring: %u\n", count);
			debugRepeat = 0;
		}
		debugRepeat++;

		int retrn;		
		if (unlikely(conf_legacyreplication)) {
			retrn = handle_replicated_packet(vec2Deq->type, vec2Deq->data, vec2Deq->len);
		} else {
			retrn = (*grt_handleStupdFunc)(vec2Deq->type, vec2Deq->data, vec2Deq->len);
		}

		if (retrn < 0)
			rte_exit(EXIT_FAILURE, "VNF notified error while handling a state update."); //TODO: maybe no complete halt here...
		rte_free(vec2Deq);
	
	}

	grt_dataplane_state = 2;

	//Free objects...

	rte_ring_free(stup_recv_ring);
#ifdef SEMAPHORE
	sem_destroy(&stup_recv_ring_deq_sem);
#endif

	//We have fully received the snapshots and all state updates. We can go over in GRT_S_WORKING state.

	uint64_t migration_duration = (rte_get_timer_cycles() - start_time_cycles) * 1000000 / rte_get_timer_hz();
	printf("mig_duration_usec=%lu\n", migration_duration);
	printf("overflown_pkt_q=%lu\n", overflown_pkt_q);

	printf("  [S] Going into GRT_S_WORKING state.\n");
	grt_state = GRT_S_WORKING;
	init_s_working();


}


//From: http://stackoverflow.com/questions/2295737/case-when-blocking-recv-returns-less-than-requested-bytes
//By user 'nos', License CC-BY-SA 3.0 https://creativecommons.org/licenses/by-sa/3.0/
//Waits exactly until specified bytes are there.
int readn(int f, void *av, int n) {
    char *a;
    int m, t;

    a = av;
    t = 0;
    while(t < n){
        m = read(f, a+t, n-t);
        if(m <= 0){
            if(t == 0)
                return m;
            break;
        }
        t += m;
    }
    return t;
}




//Handles received stup packet ENqueues only.
void* handle_stup_receive(void* nullval) {

	stup_recv_ring = rte_ring_create("stup_recv_ring", STUP_RECV_RING_SIZE, SOCKET_ID_ANY, RING_F_SC_DEQ|RING_F_SP_ENQ);
#ifdef SEMAPHORE
	sem_init(&stup_recv_ring_deq_sem, 0, 0);	//This must be freed at the end! Also the ring. But this is done by the handle_snap_receive thread at the end.
#endif

	struct sockaddr_in stup_addr = {
		.sin_family=AF_INET,
		.sin_addr={
			.s_addr=0 //0.0.0.0 will listen on all intfs (NOT secure code!). 
		},			
	};
	int status;
	struct sockaddr_in stup_rcv_clnt_addr;
	unsigned int stup_rcv_clnt_addr_len;

	stup_addr.sin_port=__bswap_16(TCP_PORT_STUP);

	sockfd_stup_rcv = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_stup_rcv < 0)
		rte_exit(EXIT_FAILURE, "Error opening socket for state update traffic. '%i'.\n", sockfd_stup_rcv); //TODO: maybe no complete halt here...

	status = bind(sockfd_stup_rcv, (struct sockaddr*)&stup_addr, sizeof(struct sockaddr_in));
	if (status < 0) rte_exit(EXIT_FAILURE, "Error binding socket for state update traffic. '%i'.\n", status); //TODO: maybe no complete halt here...

	status = listen(sockfd_stup_rcv, 1);	//only 1 connection allowed.
	if (status < 0) rte_exit(EXIT_FAILURE, "Error listening to incoming state update transfers. '%i'.\n", status); //TODO: maybe no complete halt here...
	
	stup_rcv_clnt_addr_len = sizeof(struct sockaddr_in);

	int sockfd_stup_rcv_clt = accept(sockfd_stup_rcv, (struct sockaddr*)&stup_rcv_clnt_addr, &stup_rcv_clnt_addr_len);
	if (sockfd_stup_rcv_clt < 0) {
		if (errno == EINVAL) {
			printf("   [StateUpdates] The waiting for state updates was interrupted...\n");
			close(sockfd_stup_rcv_clt);
			close(sockfd_stup_rcv);
			sockfd_stup_rcv = -1;

			//Free the state update receive ring.
			rte_ring_free(stup_recv_ring);
#ifdef SEMAPHORE
			sem_destroy(&stup_recv_ring_deq_sem);
#endif

			return 0;
		} 
		else rte_exit(EXIT_FAILURE, "Error while accepting incoming connections for state updates. '%i'.\n", errno); //TODO: maybe no complete halt here...
	}

	int bytes;
	uint64_t stats_counter = 0;
	uint64_t stats_counter_bytes = 0;
	uint64_t stats_max_queue_size = 0;
	while (1) {

		stup_vec* vec2Enq = rte_malloc("STUP_VEC", sizeof(stup_vec), 0);

		/*
		uint16_t offset2Receive = 0;
		do {
			bytes = recv(sockfd_stup_rcv_clt, vec2Enq+offset2Receive, 4-offset2Receive, 0);
			if (bytes <= 0) rte_exit(EXIT_FAILURE, "Error while receiving state update stream (vector info). ret=%i, errno=%i\n", bytes, errno); //TODO: maybe no complete halt here...
			offset2Receive +=bytes;
		} while (offset2Receive < 4);
		*/

		uint32_t bytesRead = readn(sockfd_stup_rcv_clt, vec2Enq, 4);
		if (bytesRead == 0) break;	//Finished state update receiving.
		else if (bytesRead < 4) rte_exit(EXIT_FAILURE, "Error while receiving state update stream (vector info). bytesRead=%i, errno=%i\n", 
			bytesRead, errno); //TODO: maybe no complete halt here...

		//printf("[%u,len=%u]", vec2Enq->type, vec2Enq->len);

		vec2Enq->data = rte_malloc("STUP_DATA", vec2Enq->len, 0);
		

		/*
		uint16_t offset2Receive = 0;
		do {
			bytes = recv(sockfd_stup_rcv_clt, vec2Enq->data+offset2Receive, vec2Enq->len-offset2Receive, 0);
			if (bytes <= 0) rte_exit(EXIT_FAILURE, "Error while receiving state update stream (vector payload). ret=%i, errno=%i, vecRcvd=%u, vecLen=%u.\n", bytes, errno, offset2Receive, vec2Enq->len); //TODO: maybe no complete halt here...
			offset2Receive +=bytes;
		} while (offset2Receive < vec2Enq->len);
		*/
		bytesRead = readn(sockfd_stup_rcv_clt, vec2Enq->data, vec2Enq->len);
		if (bytesRead < vec2Enq->len) rte_exit(EXIT_FAILURE, "Error while receiving state update stream (vector payload). bytesRead=%i, errno=%i\n", 
			bytesRead, errno); //TODO: maybe no complete halt here...

		int status = rte_ring_sp_enqueue(stup_recv_ring, vec2Enq);
		uint64_t currentCount = rte_ring_count(stup_recv_ring);
		if (unlikely(currentCount > stats_max_queue_size)) stats_max_queue_size = currentCount;
		if (status == -ENOBUFS) rte_exit(EXIT_FAILURE, "State update receive ring overrun. '%i'.\n", status); //TODO: maybe no complete halt here...
		stats_counter++;
		stats_counter_bytes += vec2Enq->len + 2*sizeof(uint16_t);
#ifdef SEMAPHORE
		sem_post(&stup_recv_ring_deq_sem);	//Poke the dequeue thread that something is enqueued.
#endif

	};

	printf("   [StateUpdates] Sender finished state update transmission, %lu state update vectors received in total, max_queue_size=%lu.\n", stats_counter, stats_max_queue_size);

	close(sockfd_stup_rcv_clt);
	close(sockfd_stup_rcv);
	sockfd_stup_rcv = -1;
#ifdef SEMAPHORE
	sem_post(&stup_recv_ring_deq_sem);	//Poke the dequeue thread that may hang in a wait.
#endif

	//Note: stup_recv_ring must exist after here. Nothing is added to it anymore, but it may not yet be drained.

}






void init_s_awaiting_state() {
	//Establish the listening sockets for incoming state transfer.

	if (pthread_create(&snap_rcv_thread, NULL, &handle_snapshot_receive, NULL) != 0)
		rte_exit(EXIT_FAILURE, "Could not create thread for snapshot receive.\n");

	if (pthread_create(&stup_rcv_thread, NULL, &handle_stup_receive, NULL) != 0)
		rte_exit(EXIT_FAILURE, "Could not create thread for state update receive.\n");

}

void init_s_working() {
	
	//TODO: Clear the listening sockets for state transfer.

	if (sockfd_snap_rcv != -1) {
		printf("   [Snapshotting] Closing snapshot receive thread.\n");
		shutdown(sockfd_snap_rcv, 2);
	}

	if (sockfd_stup_rcv != -1) {
		printf("   [StateUpdates] Closing state update receive thread.\n");
		shutdown(sockfd_stup_rcv, 2);
	}

}


void *handle_snapshot_transfer(void* nullval) {
	struct sockaddr_in snap_addr = { .sin_family=AF_INET, };

	snap_addr.sin_port=__bswap_16(TCP_PORT_SNAP);
	snap_addr.sin_addr.s_addr = dest_ip_buf;

	printf("   [Snapshotting] Creating socket for snapshot transfer...\n");
	sockfd_snap = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_snap < 0)
		rte_exit(EXIT_FAILURE, "Error opening socket for snapshot traffic. '%i'.\n", sockfd_snap); //TODO: maybe no complete halt here...

	printf("   [Snapshotting] Connecting via TCP to send snapshot to %x...\n", dest_ip_buf);
	if (connect(sockfd_snap,(struct sockaddr*)&snap_addr,sizeof(struct sockaddr)) < 0)
		rte_exit(EXIT_FAILURE, "Error connecting to dest for snapshot transfer: connect() failed.\n");

	//Call the VNF-dependent snapshotting function.
	
	shall_send_stupd = 1;		//From here on, we send state updates in parallel.
	int retrn = (*grt_handleSnOutFunc)();
	if (retrn < 0)
		rte_exit(EXIT_FAILURE, "VNF notified error when preparing the state snapshot."); //TODO: maybe no complete halt here...

	printf("   [Snapshotting] Finished snapshotting. %x...\n", dest_ip_buf);

	close(sockfd_snap);

}

// Must ONLY be called inside the stateout function.
//Returns the pointer to the 
int grt_addDataToSnapshot(void* data, ptrdiff_t length) {

	ptrdiff_t offset = 0;
	
	while (offset < length) {
		ssize_t bytesWritten = send(sockfd_snap, data+offset, length-offset, MSG_MORE|MSG_NOSIGNAL);
		if (bytesWritten < 0) rte_exit(EXIT_FAILURE, 
			"ERROR writing to snapshot socket: errno for stup_send_ring_deq_sem=%i, return value=%li\n", errno, bytesWritten);
		offset += bytesWritten;
		//printf("   [Snapshotting] Writing to snapshot socket: offset=%i, length=%i...\n", offset, length);
	}

	return 0;

}

// Must ONLY be called inside the statein function.
// Returns the bytes written.
// TODO: this may stronger abstract from the POSIX socket used here.
ptrdiff_t grt_getDataFromSnapshot(void* bufferToWriteInto, ptrdiff_t maxBytesToWrite) {

	ptrdiff_t offset = 0;
	while (offset < maxBytesToWrite) {
		ssize_t bytes = recv(sockfd_snap_rcv_clt, bufferToWriteInto+offset, maxBytesToWrite-offset, 0);
		if (bytes == 0) return offset;	//Stream was closed.
		if (bytes < 0) rte_exit(EXIT_FAILURE, "ERROR: There was an error when receiving data from the snapshot: errno=%i\n", errno);
		offset += bytes;
		
	}

	//rte_exit(EXIT_FAILURE, "ERROR: Target snapshot buffer exceeded. Closing for security.");

	uint64_t addlBytes = 0;
	uint8_t trashBuf[4096];
	ssize_t bytes;
	do {
		//TODO: This could be removed in practice, but is there to remove the "simulated" snapshot data.
		//TODO: more beautiful with large hugepages. Move to app?
		bytes = recv(sockfd_snap_rcv_clt, &trashBuf, 4096, 0);
		if (bytes < 0) rte_exit(EXIT_FAILURE, "ERROR: There was an error when receiving data from the snapshot: errno=%i\n", errno);
		addlBytes += bytes;
		
	} while (bytes > 0);
	
	printf("   [Snapshotting] Received %lu additional bytes after snapshot buffer was depleted.\n", addlBytes);

	

}

// Returns  > 0 if stupds shall be sent.
int grt_shall_notify_stupd() {
	if (shall_send_stupd > 0 && conf_legacyreplication == 0) return 1;
	else return 0;
}

// Returns  > 0 if stupds shall be sent.
int grt_shall_notify_stupd_legacy() {
	if (shall_send_stupd > 0 && conf_legacyreplication != 0) return 1;
	else return 0;
}

//Normally returns true, currently only not if legacy replication is enabled.
int grt_shall_output() {
	if (unlikely(conf_legacyreplication != 0 && grt_dataplane_state < 2)) return 0;
	else return 1;
}



int _grt_notify_stupd_internal(uint16_t type, void* byteVectorObj, uint16_t length) {

	//Quickly returns in order not to degrade performance.

	if (unlikely(length > grt_MAX_STUPD_LEN)) rte_exit(EXIT_FAILURE, "    [StateUpdates] ERROR: State update size currently limited to %i byte.\n", grt_MAX_STUPD_LEN);
	stup_vec* vec2Enq = rte_malloc("STUP_VEC", sizeof(stup_vec), 0);
	vec2Enq->type = type;
	vec2Enq->data = byteVectorObj;
	vec2Enq->len = length;
	if (vec2Enq->len == 0) printf("WARNING: Length of vec2enq is 0!");
	int status = rte_ring_mp_enqueue(stup_send_ring, vec2Enq);
#ifdef SEMAPHORE
	sem_post(&stup_send_ring_deq_sem);	//Poke the dequeue thread that something is enqueued.
#endif
	return 0;

}

// The array oof byte vectors notified is guaranteed 
// to be received by a VM to update in the same order 
//and chunk length.
//ONLY to be called if grt_shall_notify_stupd() returns something > 0

int grt_notify_stupd(uint16_t type, void* byteVectorObj, uint16_t length) {

	if (unlikely(shall_send_stupd <= 0)) {
		printf("  [S] State update received, but shall not send state updates. Ignoring, but please fix that.\n");
		return -1;
	}

	if (unlikely(conf_legacyreplication != 0)) {
		printf("  [S] State update received, but only shall send legacy state updates. Ignoring.\n");
		return -1;
	}

	return _grt_notify_stupd_internal(type, byteVectorObj, length);

}

//Legacy mode to compare with previous work

int grt_notify_stupd_legacy(uint16_t type, void* byteVectorObj, uint16_t length) {

	if (unlikely(shall_send_stupd <= 0)) {
		printf("  [S] State update received, but shall not send state updates. Ignoring, but please fix that.\n");
		return -1;
	}

	if (unlikely(conf_legacyreplication == 0)) {
		printf("  [S] State update received, but only shall NOT send legacy state updates. Ignoring.\n");
		return -1;
	}

	return _grt_notify_stupd_internal(type, byteVectorObj, length);

}


void *stup_send_dequeue_loop(void* nullparam) {

	stup_vec* vec2Deq;

	while(1) {

		int status = rte_ring_sc_dequeue(stup_send_ring, (void**)&vec2Deq);
		//printf("   [StateUpdates] Trying to dequeue something from stup send queue. Status %i \n", status);
		if (status != 0) {
			if (shall_send_stupd < 0) {
				//State updating is finished. We can safely stop the dequeue thread, as everything has been dequeued.
				break;
			}
#ifdef SEMAPHORE
			sem_wait(&stup_send_ring_deq_sem);	//Blocks until something has been enqueued
#endif
			continue;
		}
			
		//printf("   [StateUpdates] Successfully dequeued object of length %u \n", vec2Deq->len);

		uint16_t currentOffset = 0;

		int bytesWritten;

		do {
			bytesWritten = send(sockfd_stup, vec2Deq+currentOffset, 4-currentOffset, MSG_MORE|MSG_NOSIGNAL);
			if (bytesWritten <= 0) rte_exit(EXIT_FAILURE, "    [StateUpdates] ERROR: Connection closed while sending state update metadata. errno=%i\n", errno);
			currentOffset += bytesWritten;
		} while (currentOffset < 4);

		currentOffset = 0;

		do {
			bytesWritten = send(sockfd_stup, vec2Deq->data+currentOffset, vec2Deq->len-currentOffset, MSG_MORE|MSG_NOSIGNAL);
			if (bytesWritten <= 0) rte_exit(EXIT_FAILURE, "    [StateUpdates] ERROR: Connection closed while sending state update data. errno=%i\n", errno);
			currentOffset += bytesWritten;
		} while (currentOffset < vec2Deq->len);

		//printf("   [StateUpdates] Freeing byte vector %s at %p, in container struct %p\n", (char*)vec2Deq->data, vec2Deq->data, vec2Deq);
		rte_free(vec2Deq->data);	//Byte vector has been finally used, now free it.
		//printf("   [StateUpdates] Freeing overall struct %u \n", vec2Deq->len);
		rte_free(vec2Deq);	//Free also the vec struct.

	}
	
	printf("   [StateUpdates] Finished state update sending.\n");

	shall_send_stupd = 0;

	rte_ring_free(stup_send_ring);
#ifdef SEMAPHORE
	sem_destroy(&stup_send_ring_deq_sem);
#endif
	close(sockfd_stup);
	sockfd_stup = -1;

	printf("  [S] Going into GRT_S_AWAITING_STATE.\n");

	grt_state = GRT_S_AWAITING_STATE;
	init_s_awaiting_state();

}


/*
 Handles everything for state transfer (providing node).
 Assumes the state has switched to GRT_S_UPD_PROV.
*/
void init_s_upd_prov(uint32_t dest_ip, char* dest_ip_str) {
	
	//Prepare rings for state updates

	printf("   [StateTransfer] Creating ring for buffered STUP sending...\n");

	stup_send_ring = rte_ring_create("stup_send_ring", STUP_SEND_RING_SIZE, SOCKET_ID_ANY, RING_F_SC_DEQ); //TODO: NUMA awareness?
#ifdef SEMAPHORE
	sem_init(&stup_send_ring_deq_sem, 0, 0);	//TODO: This must be freed at the end! Also the ring!
#endif

	//Note: We need to do prepare the connection for state updates BEFORE we start the snapshotting.
	//Otherwise, it might not be open when it is needed after the START of snapshotting.

	stup_addr.sin_port=__bswap_16(TCP_PORT_STUP);
	stup_addr.sin_addr.s_addr = dest_ip;

	printf("   [StateTransfer] Creating socket for state update transfer...\n");
	sockfd_stup = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_stup < 0)
		rte_exit(EXIT_FAILURE, "Error opening socket for state update traffic. '%i'.\n", sockfd_stup); //TODO: maybe no complete halt here...	

	printf("   [StateTransfer] Connecting via TCP to dest for state updates at %s...\n", dest_ip_str);
	if (connect(sockfd_stup,(struct sockaddr*)&stup_addr,sizeof(struct sockaddr)) < 0)
		rte_exit(EXIT_FAILURE, "Error connecting to controller: connect() failed.\n");

	shall_send_stupd = 0;	//If this remains -1, the thread created in the following might be immediately killed in rare cases.

	//Move the TCPing of state update packets to another thread (mutex and performance benefits).
	dest_ip_buf = dest_ip;
	printf("   [StateTransfer] Push TCPing of STUPs to another thread...\n");
	if (pthread_create(&stup_tcping_thread, NULL, &stup_send_dequeue_loop, NULL) != 0)
		rte_exit(EXIT_FAILURE, "Could not create thread for TCPing.\n");

	//Now SNAPSHOTTING in another thread.
	dest_ip_buf = dest_ip;
	printf("   [StateTransfer] Push snapshotting to another thread...\n");
	if (pthread_create(&snap_thread, NULL, &handle_snapshot_transfer, NULL) != 0)
		rte_exit(EXIT_FAILURE, "Could not create thread for state transfer.\n");


	//At the end, the snapshotting is running, and the socket for state updates is open. We now can receive control messages again.

}

void generateStats() {

	
	for (int i = 0; i < fwd_lcores_c; i++) {	//Maximum index of available lcores.
		printf("Debug field for intf %i is %lu.\n", i, grt_lcore_info[i]->debug);

		struct grt_lcore_info_s* info;
		info = grt_lcore_info[i];	//Map: available-lcore-id -> general lcore_id
		if (info == NULL) continue;
		if (info->mempool == NULL) continue;
		uint64_t count = rte_mempool_count(info->mempool);
		uint64_t free_count = rte_mempool_free_count(info->mempool);
		printf("====== MEMPOOL count: %lu, free_count: %lu, lcore %u \n", count, free_count, info->lcore_id);
		rte_mempool_list_dump(stdout);
	}

}


void *ctrl_loop(void* nullval) {

	ctrl_msg in_buf;

	char ipv4_addr_buf[20];

	while (1) {
		int n = recv(sockfd_ctrl, (char*)&in_buf, 8, 0);	//only 1 ctrl (8 bytes) msg at once. TODO: Any flags to set here?
		if (n == 0)
			rte_exit(EXIT_FAILURE, "Controller has closed the connection. Was this intentional?\n");
		if (n < 8) {
			printf("  [S] Malformed packet received.");
			continue;
		}

		uint16_t opcode = __bswap_16(in_buf.opcode);
		printf("  [S] Controller has sent message. opcode='%x'.\n", opcode);

		if (opcode == CTRL_MSG_POKE) {
			printf("  [S] Controller has sent CTRL_MSG_POKE, poke UID %u.\n", __bswap_32(in_buf.addr));
			generateStats();
			send_ctrl_msg(&in_buf);	//"Reflects" the 8-byte poke packet with all its unused contents.

		} else if (opcode == CTRL_MSG_STATE_TRANSFER) {
			grt_ipv4_to_string(ipv4_addr_buf, __bswap_32(in_buf.addr));	//DPDK seems to be LE, this here BE !?!?
			printf("  [S] Controller has sent CTRL_MSG_STATE_TRANSFER, addr='%s'.\n", ipv4_addr_buf);
			if (grt_state != GRT_S_WORKING) {
				printf("  [S] ERROR: Message CTRL_MSG_STATE_TRANSFER received, although not in WORKING state. Ignoring.\n");
				continue;
			}
			grt_state = GRT_S_UPD_PROV;
			init_s_upd_prov(in_buf.addr, ipv4_addr_buf);
				
		} else if (opcode == CTRL_MSG_INIT) {
			printf("  [S] Controller has sent CTRL_MSG_INIT.\n");
			if (grt_state != GRT_S_AWAITING_STATE) {
				printf("  [S] ERROR: Message CTRL_MSG_INIT received, although not in AWAITING_STATE. Ignoring.\n");
				continue;
			}
			grt_dataplane_state = 2;
			grt_state = GRT_S_WORKING;
			init_s_working();
				
		} else {
			printf("  [S] Controller has sent unknown opcode.\n");
		}

	}

}

void grt_init_ctrl_thread(char* server_ip_str) {

	printf("  [S] Parsing string address to struct...\n");
	if (inet_aton(server_ip_str, &serv_addr.sin_addr) == 0)
		rte_exit(EXIT_FAILURE, "Could not parse controller inet address '%s'.\n", server_ip_str);

	serv_addr.sin_port=__bswap_16(TCP_PORT_CTRL);	//set port.

	printf("  [S] Creating socket for ctrl...\n");
	sockfd_ctrl = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd_ctrl < 0)
		rte_exit(EXIT_FAILURE, "Error opening socket for control traffic. '%i'.\n", sockfd_ctrl);

	printf("  [S] Connecting via TCP to controller at %s...\n", server_ip_str);
	if (connect(sockfd_ctrl,(struct sockaddr*)&serv_addr,sizeof(struct sockaddr)) < 0)
		rte_exit(EXIT_FAILURE, "Error connecting to controller: connect() failed.\n");
	
	printf("  [S] Push the rest to another thread...\n");
	if (pthread_create(&ctrl_thread, NULL, &ctrl_loop, NULL) != 0)
		rte_exit(EXIT_FAILURE, "Could not create controller thread.\n");

	grt_state = GRT_S_AWAITING_STATE;
	init_s_awaiting_state();

}









