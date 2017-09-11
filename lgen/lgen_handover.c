#include <stdio.h>
#include <string.h>
#include <byteswap.h>
#include <malloc.h>
#include <sys/types.h>
#include <inttypes.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <endian.h>

//Multi-threaded load generator script to test the performance of the GRT application. 
//Like lgen_main.c, but appends special metadata and restrictive tests for the handover NF.
//Data obtained should be postprocessed by eval_rtt_ploss.py


uint16_t packetsz;
uint64_t conn_send_intvl_mean;
uint32_t cell_change_mean_msec;
char* output_path;
uint16_t port;
uint16_t multiport;
uint16_t runtime;

int stopped = 0;
int stopped_rcv = 0;
uint64_t totalPktsSent =0;
uint64_t totalPktsRcvd =0;

uint64_t startTimeNsec;

//Segment size = packet size -42. 14(Eth)+20(IPv4)+8(UDP) =42.
#define HEADERS_SIZE		42

#define PREAMBLE		0xd066f00d
#define SEC_TO_NSEC		1E9
#define PKTBUF_SZ		1024*16
#define FILENAME_BUF_SZ 	1024
#define DUMP_STATS_INTVL	2
//#define FLAT_INTERVAL

#define TOLERANCE_TIME		7500000//In nanoseconds:	50% more allowed.

//#define DROP_TEST
//#define SHOW_DOT_PROGRESS

typedef struct __attribute__((__packed__)) {
	uint32_t preamble;
	uint16_t stream_id;
	uint32_t packet_c;
	uint64_t timestamp;
	uint16_t cellId;
	uint8_t update;		//0 means client-side, no cell change, 1 client side, cell change, 2 means network-side, processed by NF.
} in_packet_info_t;

typedef struct {
	uint16_t thread_no;
	int thread_sock;
	struct sockaddr_in server_addr;
	uint64_t lastCellIdChange;
	uint16_t lastCellId;
	uint16_t currentCellId;	//0 if not malicious, != 0 otherwise.
} threadinfo_t;

//All little-endian!
typedef struct __attribute__((__packed__)) {
	uint16_t thread_no;
	uint32_t packet_c;
	uint64_t abs_snd_time;
} file_field_snd_t;

//All little-endian!
typedef struct __attribute__((__packed__)) {
	uint16_t thread_no;
	uint32_t packet_c;
	uint64_t rtt_nsec;
} file_field_t;


void memPrintHex(void* start, char* preamble, char* postamble, uint32_t len) {
	fprintf(stderr, "%s", preamble);
	int i;
	for(i=0; i<len;i++) fprintf(stderr, "%02x ", ((unsigned char*)start)[i]);
	fprintf(stderr, "%s", postamble);
}

uint32_t getNewSleepTime() {
	if (conn_send_intvl_mean == 0) return 0;
#ifdef FLAT_INTERVAL
	uint32_t result = conn_send_intvl_mean;
#else
	uint32_t result = rand() % (conn_send_intvl_mean*2);
#endif
	//printf("New random sleep time %u.\n", result);
	return result;
}

uint64_t getTimeNanoseconds() {
	struct timespec currentTime;
	clock_gettime(CLOCK_REALTIME, &currentTime);
	return currentTime.tv_sec*SEC_TO_NSEC + currentTime.tv_nsec;
}

void handle_rcv_thread(void* ptr) {

	threadinfo_t* threadinfo = (threadinfo_t*)ptr;

	//printf("Successfully opened rcv thread %u\n", threadinfo->thread_no);

	char filename[FILENAME_BUF_SZ];
	snprintf(filename, FILENAME_BUF_SZ, "%s/lgen_%u.grtlog-rcv", output_path, threadinfo->thread_no);

	FILE* logfile = fopen (filename, "wb");
	if (logfile == NULL) {
		fprintf(stderr, "Thread %i: Could not open file '%s' Does the path exist?\n", 
				threadinfo->thread_no, filename);
		return;
	}
	file_field_t filefield;
	filefield.thread_no = __bswap_16(threadinfo->thread_no);

#ifdef DROP_TEST
	uint32_t drop_ctr = 0;
#endif

	while(stopped_rcv == 0) {
		void* buf[PKTBUF_SZ];

		ssize_t bytesWritten = recv(threadinfo->thread_sock, &buf, PKTBUF_SZ, 0);

#ifdef DROP_TEST
		if (drop_ctr++ >= 2) {
			drop_ctr = 0;
			continue;
		}
#endif

		uint64_t abs_rcv_time = getTimeNanoseconds();
		if (bytesWritten != packetsz-HEADERS_SIZE) {
			fprintf(stderr, "Thread %i: Wrong-sized packet received: %li, should be %u. Dropping.\n", 
				threadinfo->thread_no, bytesWritten, packetsz);
			continue;
		}
		
		in_packet_info_t* info = (in_packet_info_t*)buf;

		if (info->preamble != __bswap_32(PREAMBLE)) {
			fprintf(stderr, "Thread %i: Packet did not have correct preamble. Dropping.\n", 
				threadinfo->thread_no);
			continue;
		}
		
		if (info->cellId != threadinfo->currentCellId) {
		  
		  	if (info->update != 2) {
			    fprintf(stderr, "Thread %i: Packet has not been processed by the NF. Considering it as lost. \n", threadinfo->thread_no);
			    continue;
			    
			}
			if (info->cellId != threadinfo->lastCellId || threadinfo->lastCellIdChange + TOLERANCE_TIME < abs_rcv_time) {
			    //fprintf(stderr, "Thread %i: Packet was for wrong cell. Considering it as lost. \n", threadinfo->thread_no);
			    fprintf(stderr, "X");
			    continue;
			    
			}
			
			//fprintf(stderr, "Thread %i: Warning: Packet was for previous cell, but in tolerance time. \n", threadinfo->thread_no);	
		}

		//filefield.abs_rcv_time = __bswap_64(abs_rcv_time);
		filefield.rtt_nsec = __bswap_64(abs_rcv_time - __bswap_64(info->timestamp));	//nsec to usec

		//fprintf(stderr, "Thread %i: Correct packet received, rtt=%lu rttHex=0x%lx nsec.\n", 
		//		threadinfo->thread_no, filefield.rtt_nsec, filefield.rtt_nsec);
#ifdef SHOW_DOT_PROGRESS
		fprintf(stdout, ".");
		fflush(stdout);
#endif

		//uint32_t packet_c = __bswap_32(info->packet_c);
		filefield.packet_c = info->packet_c;

		//memPrintHex((void*)&filefield, "(", ")\n", sizeof(file_field_t));
	
		totalPktsRcvd++;

		fwrite((void*)&filefield, sizeof(file_field_t), 1, logfile);
	}

	fclose(logfile);


}

uint16_t newCellId() {
  return rand();
}

//In nanoseconds
uint64_t getTimeUntilNextChange() {
  return ((((uint64_t)rand()) % ((uint64_t)cell_change_mean_msec)))*2000000;	//double for rand, and msec to nsec
}


void handle_thread(void* ptr) {

	threadinfo_t* threadinfo = (threadinfo_t*)ptr;

	//printf("Successfully opened thread %u\n", threadinfo->thread_no);

	char filename[FILENAME_BUF_SZ];
	snprintf(filename, FILENAME_BUF_SZ, "%s/lgen_%u.grtlog-snd", output_path, threadinfo->thread_no);

	FILE* logfile = fopen (filename, "wb");
	if (logfile == NULL) {
		fprintf(stderr, "Thread %i: Could not open file '%s' Does the path exist?\n", 
				threadinfo->thread_no, filename);
		return;
	}
	file_field_snd_t filefield;
	filefield.thread_no = __bswap_16(threadinfo->thread_no);

	uint32_t packet_c = 0;
	uint64_t nextCellChange = getTimeUntilNextChange();
	uint64_t nextPacketAt = getTimeNanoseconds();

	threadinfo->lastCellId = 0;
	threadinfo->currentCellId = newCellId();
	
	while(stopped == 0) {

		nextPacketAt += getNewSleepTime()*1E3;	//nsec, random sleep time in milliseconds
		uint64_t currentTime = getTimeNanoseconds();
		if (nextPacketAt > currentTime) {
			uint64_t sleeptime = nextPacketAt - currentTime;
			//printf("Sleeping for %lu nsec\n", sleeptime);
			usleep(sleeptime/1000);
		} else {
			//printf("Not sleeping, because behind schedule.\n");
		}
		
		//fprintf(stdout, "Thread %i: Sleeping...\n", threadinfo->thread_no);
		//printf("Last job duration: %lu nsec, sleeping for %li usec.\n", lastjob_duration, newRandomSleepTime);

		//fprintf(stdout, "Thread %i: Creating packet...\n", threadinfo->thread_no);
		in_packet_info_t* buf = calloc(1, packetsz-HEADERS_SIZE);	//TODO ensure packetsz not less than sizeof struct.
		
		currentTime = getTimeNanoseconds();
		
		if (currentTime > nextCellChange) {
		  threadinfo->lastCellId = threadinfo->currentCellId;
		  threadinfo->currentCellId = newCellId();
		  threadinfo->lastCellIdChange = currentTime;
		  buf->update = 1;
		  uint64_t next_intvl = getTimeUntilNextChange();
		  //printf("New time until next chang3: %lu.\n", next_intvl);
		  nextCellChange = currentTime + next_intvl;
		}

		buf->preamble = __bswap_32(PREAMBLE);
		buf->stream_id = __bswap_16(threadinfo->thread_no);
		buf->packet_c = __bswap_32(packet_c);
		filefield.packet_c = buf->packet_c;

		//fprintf(stdout, "Thread %i: Getting time...\n", threadinfo->thread_no);
		buf->timestamp = __bswap_64(getTimeNanoseconds());
		buf->cellId = threadinfo->currentCellId;
		filefield.abs_snd_time = buf->timestamp;
		//fprintf(stdout, "Thread %i: Time: %lu nsec\n", threadinfo->thread_no, __bswap_64(buf->timestamp));

		//fprintf(stdout, "Thread %i: Sending...\n", threadinfo->thread_no);


		if (sendto(threadinfo->thread_sock, buf, packetsz-HEADERS_SIZE, 0, (struct sockaddr*)&threadinfo->server_addr, sizeof(struct sockaddr)) == -1) {
			fprintf(stderr, "Thread %i: Could not send packet.\n", threadinfo->thread_no);
			return;
		}

		free(buf);

		fwrite((void*)&filefield, sizeof(file_field_snd_t), 1, logfile);
		totalPktsSent++;

		packet_c++;

		//TODO: Subtract working time from next sleeping time.

	}

	fclose(logfile);

}

void dumpStats() {

	uint64_t elapsedTime = getTimeNanoseconds() - startTimeNsec;
	int64_t sndRcvDiffPkt = totalPktsSent-totalPktsRcvd;

	printf("Sent %lu p, %f p/s, %lu byte, %f byte/s %f bit/s\n", totalPktsSent, totalPktsSent*1E9/elapsedTime, 
		totalPktsSent*packetsz, totalPktsSent*packetsz*1E9/elapsedTime, totalPktsSent*packetsz*8*1E9/elapsedTime);
	printf("Received %lu p, %f p/s, %lu byte, %f byte/s %f bit/s\n", totalPktsRcvd, totalPktsRcvd*1E9/elapsedTime, 
		totalPktsRcvd*packetsz, totalPktsRcvd*packetsz*1E9/elapsedTime, totalPktsRcvd*packetsz*8*1E9/elapsedTime);
	printf("Diff snd/rcv %li p\n", sndRcvDiffPkt);
}

void dumpStatsThread() {
	while(1) {
		sleep(DUMP_STATS_INTVL);
		dumpStats();
	}
}

int main(int argc, char **argv) {

	if (argc < 7) {
		printf("Usage: %s <outputPath> <num_connections> <packetsz(bytes)> <conn_send_intvl_mean(usec)> <runtime(seconds)> <cell_change_mean_msec>\n", argv[0]);
		return -1;
	}

	output_path = argv[1];
	uint64_t num_connections = atoi(argv[2]);
	packetsz = atoi(argv[3]);		//Must be > 32
	conn_send_intvl_mean = atoi(argv[4]);		//Must be > 10
	runtime = atoi(argv[5]);
	cell_change_mean_msec = atoi(argv[6]);
	char* server_ip_str = "192.168.1.2";
	port = 8000;
	multiport = 4;

	if (packetsz < HEADERS_SIZE + sizeof(in_packet_info_t)) {
		printf("Error: Packet size too small.");
		exit(-10);
	}

	pthread_t* pthreads_list = malloc(sizeof(pthread_t) * num_connections);
	pthread_t* pthreads_list_rcv = malloc(sizeof(pthread_t) * num_connections);

	startTimeNsec = getTimeNanoseconds();

	uint16_t i;
	for (i = 0; i < num_connections; i++) {

		threadinfo_t* threadinfo = malloc(sizeof(threadinfo_t));
		threadinfo->thread_no = i;

		memset((char*) &threadinfo->server_addr, 0, sizeof(struct sockaddr_in));
		threadinfo->server_addr.sin_family = AF_INET;
		if (inet_aton(server_ip_str, &threadinfo->server_addr.sin_addr) == 0) {
			fprintf(stderr, "Server IP parsing failed\n");
			return -2; 
		}
		threadinfo->server_addr.sin_port = htons(port);
		if (multiport > 1) threadinfo->server_addr.sin_port = htons(port+i%multiport);

		if ((threadinfo->thread_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
			fprintf(stderr, "Thread %i: Could not create socket.\n", i);
			return -2;
		}

		//int val = 1;
		//setsockopt(threadinfo->thread_sock, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val));
		int val = IP_PMTUDISC_DO;
		setsockopt(threadinfo->thread_sock, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));

		if (pthread_create(&pthreads_list[i], NULL, (void*) &handle_thread, (void*)threadinfo) != 0)
			printf("Thread %i: Could not create thread for packet sending.\n", i);

		if (pthread_create(&pthreads_list_rcv[i], NULL, (void*) &handle_rcv_thread, (void*)threadinfo) != 0)
			printf("Thread %i: Could not create thread for packet reception.\n", i);
	}

	pthread_t stats_thread;
	if (pthread_create(&stats_thread, NULL, (void*) &dumpStatsThread, NULL) != 0)
		printf("Thread %i: Could not create thread for packet sending.\n", i);

	sleep(runtime);

	stopped = 1;
	dumpStats();

	sleep(3);

	stopped_rcv = 1;

	sleep(1);

	return 0;

}

