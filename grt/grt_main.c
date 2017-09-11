#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>

#include <sys/queue.h>
#include <sys/stat.h>

#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>

#include <libxml/xmlreader.h>

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
#include "grt_main.h"
#include "grt_toolz.h"

//======= Pre-Definitions

//#define TEST_HASH_ONLY

#define RTE_TEST_RX_DESC_DEFAULT 32768
#define RTE_TEST_TX_DESC_DEFAULT 512
#define MAX_SIZE_BURST 32
#define MEMPOOL_CACHE_SIZE 256

//======= Definitions

#define CACHE_LINE_SIZE		64
#define UINT8_UNDEF		255
#define grt_CORE_IDS_MAX	255
#define RTE_MAX_IFACE		255

#define MBUF_SIZE (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)	//changed from 1024 to 2048 for RX performance.


//======= Structs =========


typedef uint8_t lcoreid_t;
typedef uint8_t streamid_t;
typedef uint8_t queueid_t;



//=========================

uint8_t fwd_lcores_c;	//Maximum index of available lcores.
uint8_t fwd_available_lcores[RTE_MAX_LCORE];	//Map: available-lcore-id -> general lcore_id
uint8_t lcore_to_available_idx[RTE_MAX_LCORE];	//Map: general lcore_id -> available-lcore-id
struct grt_lcore_info_s* grt_lcore_info[RTE_MAX_LCORE];	//Struct: available-lcore-id -> some infos.
struct grt_interface_info* grt_interfaces[RTE_MAX_IFACE];

uint8_t devs_port_ids[255];	//Maps the SliM device number (according to devs_macs) to the port_id

handlePktPtr grt_handlePktFunc;
handleSnOutPtr grt_handleSnOutFunc;
handleSnInPtr grt_handleSnInFunc;
handleStupdPtr grt_handleStupdFunc;

uint8_t num_devs = 0;

char* devs_macs[255];		//All the mac addresses to look up in the available intfs.
uint8_t devs_macs_len = 0;

uint32_t conf_artificialsnapsz = 0;	//In 4K-blocks!
int32_t conf_legacyreplication = 0;	// != 0: Use legacy.

uint8_t mac_filter_fix = 0; 		//1 if enabled. TODO: maybe this must be moved to the app.

struct rte_mempool* grt_replication_mempool;	//Only needed for legacy replication

uint8_t conf_ignoremacs = 0; // Set to != 0 if MAC address interface assignment shall be ignored.

#include "internal_tools.c"
#include "grt_state_and_control.c"
#include "grt_dataplane.c"

#ifdef TEST_HASH_ONLY
#include "test/grt_redirect_table_test.c"
#endif

uint8_t is_macfilterfix_enabled() {
	return mac_filter_fix;
}

int load_config() {
    	xmlTextReaderPtr reader;
   	int status;

	reader = xmlReaderForFile("goldenretriever.xml", NULL, 0);
   	if (reader == NULL) {
		printf("[CONF] Unable to open config file.\n");
		return -1;
	}
       	status = xmlTextReaderRead(reader);
        while (status == 1) {


        	const xmlChar *name = xmlTextReaderConstName(reader);
		//printf("Parsing name '%s'\n", name);
		if (name == NULL) continue;

		if (strcmp(name, "ignoremacs") == 0) {
		  conf_ignoremacs = 1;
		}
		
		
		if (strcmp(name, "interface") == 0) {
			
			const xmlChar* value = xmlTextReaderGetAttribute(reader, "address");

			if (value == NULL) {
				printf("[CONF] An interface must have the address attribute.\n");
				return -2;            	
				//Parse error
			}

			devs_macs[devs_macs_len] = malloc(strlen(value)+1);
			strcpy(devs_macs[devs_macs_len++], value);

			printf("[CONF] Found interface: name='%s', value='%s'\n", name, value);
		
		}

		if (strcmp(name, "mac-filter-fix") == 0) {
			
			const xmlChar* value = xmlTextReaderGetAttribute(reader, "enabled");

			if (value == NULL) {
				printf("[CONF] \"mac-filter-fix\" should carry the enabled attribute (\"true\" if enabled).\n");
				return -2;            	
				//Parse error
			}

			if (strcmp("true", value) == 0) {
				mac_filter_fix = 1;
				printf("[WARN] Enabled macFilterFix\n");
			}
		
		}

       		status = xmlTextReaderRead(reader);
        }
       	xmlFreeTextReader(reader);
        if (status != 0) {
		printf("[CONF] Parse error in XML document.\n");
		return -2;            	
		//Parse error
    	}

	return status;

}


int grt_main(int argc, char **argv, prepareInEALPtr prepareInEAL,
			handlePktPtr handlePktFunc, 
			handleSnOutPtr handleSnOutFunc, 
			handleSnInPtr handleSnInFunc,
			handleStupdPtr handleStupdFunc) {

	if (load_config() != 0) {
		printf("[CONF] Error while loading config file.\n");
		exit(123);
	}

	char* ctrl_ip_address = "172.16.20.2"; //TODO make input parameter	

	//devs_macs = devs_macs_init;	

	static struct rte_eth_conf default_ethconf = {
		.link_speeds = ETH_LINK_SPEED_AUTONEG,
		.rxmode = {
			.mq_mode = ETH_MQ_RX_NONE,
			.max_rx_pkt_len = ETHER_MAX_VLAN_FRAME_LEN,
			.split_hdr_size = 0,
			.header_split = 0,
			.hw_ip_checksum = 0,
			.hw_vlan_filter = 0,
			.hw_vlan_strip = 0,
			.hw_vlan_extend = 0,
			.jumbo_frame = 0,
			.hw_strip_crc = 0,
			.enable_scatter = 0,
			.enable_lro = 0,
		},
		.txmode = {
			.mq_mode = ETH_MQ_TX_NONE,
			.hw_vlan_reject_tagged = 0,
			.hw_vlan_reject_untagged = 0,
			.hw_vlan_insert_pvid = 0,
		},
		.lpbk_mode = 0,
		.rx_adv_conf = {
			.rss_conf = {		//Receive Side Scaling.
				.rss_key = NULL,
				.rss_key_len = 0,
				.rss_hf = 0,
			},
		},
	};
		
	static const struct rte_eth_rxconf rx_conf = {
		.rx_thresh = {
			.pthresh = 8,	//prefetch
			.hthresh = 8,	//host
			.wthresh = 4	//write-back
		},
		.rx_free_thresh = 32,
	};	

	static struct rte_eth_txconf tx_conf = {
		.tx_thresh = {
			.pthresh = 36,
			.hthresh = 0,
			.wthresh = 0
		},
		.tx_free_thresh = 0,
		.tx_rs_thresh = 0,
		.txq_flags = (ETH_TXQ_FLAGS_NOMULTSEGS |
			ETH_TXQ_FLAGS_NOVLANOFFL |
			ETH_TXQ_FLAGS_NOXSUMSCTP |
			ETH_TXQ_FLAGS_NOXSUMUDP |
			ETH_TXQ_FLAGS_NOXSUMTCP)

	};

	grt_handlePktFunc = handlePktFunc;
	grt_handleSnOutFunc = handleSnOutFunc;
	grt_handleSnInFunc = handleSnInFunc;
	grt_handleStupdFunc = handleStupdFunc;


	//===== END CONFIG

	int status;

	printf("[[I]] Starting DPDK EAL...\n");
	
	status = rte_eal_init(argc, argv);
	if (status < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	}

	#ifdef TEST_HASH_ONLY

	printf("[[I]] Starting hashtable test instead...\n");
	test_main();
	exit(0);

	#endif

	int retrn = (*prepareInEAL)();
	printf("[[I]] VNF EAL Prepare returned %i.\n", retrn);

	printf("[[I]] Initiating control and state management...\n");

	grt_init_ctrl_thread(ctrl_ip_address);


	printf("[[I]] Checking for ports...\n");

	uint8_t rte_devcount = rte_eth_dev_count();

	if (rte_devcount == 0) {
		rte_exit(EXIT_FAILURE, "No probed ethernet devices\n");
		printf("[[I]] No devs, exiting\n");
	}

	printf("[[I]] Found %i net devices.\n", rte_devcount);
	



	//Get the CPU IDs
	
	uint8_t i;
	for (i=0; i < RTE_MAX_LCORE; i++) {
		
		//printf("Core: %i, isEnabled: %i\n", i, rte_lcore_is_enabled(i)); 
		if (rte_lcore_is_enabled(i) && !(rte_get_master_lcore() == i)) {
			//printf("  Adding.\n");
			fwd_available_lcores[fwd_lcores_c] = i;
			lcore_to_available_idx[i] = fwd_lcores_c++;
		}
	}

	grt_printIntArray(fwd_available_lcores, 30);
	grt_printIntArray(lcore_to_available_idx, 30);

	printf("[[I]] Forwarding CPUs: ");
	grt_printIntArray(fwd_available_lcores, fwd_lcores_c);
	printf(". Master Core (not forwarding): %i\n", rte_get_master_lcore());

	printf("[[I]] Ignoremacs set to %u\n", conf_ignoremacs);
	num_devs = (conf_ignoremacs == 0)?devs_macs_len:rte_devcount;
	
	if (fwd_lcores_c < num_devs) {
		rte_exit(EXIT_FAILURE, "SliM requires at least as many cores as it has interfaces to do the job. Cores: %i, Devices: %i\n", fwd_lcores_c, rte_devcount);
	}

	argc -= status;
	argv += status;
	
	//Here, command parsing would come.

	//Args: [conf_artificialsnapsz = 0] [conf_legacyreplication = 0, !=0 if yes.]
	
	if (argc > 1) {
		conf_artificialsnapsz = atoi(argv[1]);
	}
	if (argc > 2) {
		conf_legacyreplication = atoi(argv[2]);
	}
	printf("[[I]] Initializing SliM with args [conf_artificialsnapsz = %u] [conf_legacyreplication = %i]. argc=%i\n", conf_artificialsnapsz, conf_legacyreplication, argc);


	
	//We initialize the lcore struct array, except the mempool

	for (i=0; i < fwd_lcores_c; i++) {
		grt_lcore_info[i] = rte_zmalloc("grt: struct grt_lcore_info", sizeof(struct grt_lcore_info_s), CACHE_LINE_SIZE);
		if (grt_lcore_info[i] == NULL) rte_exit(EXIT_FAILURE, "Could not alloc mem for grt_lcore_info %i", i);
		grt_lcore_info[i]->lcore_id = i;
	}


	//Find the interior and exterior interface by MAC address...
	printf("[[I]] Associating devices...\n");

	for(i=0; i<devs_macs_len; i++) {
		devs_port_ids[i] = UINT8_UNDEF;
		//struct grt_interface_info grt_interfaces[RTE_MAX_IFACE];
		grt_interfaces[i] = rte_zmalloc("grt: struct grt_interface_info", sizeof(struct grt_interface_info), CACHE_LINE_SIZE);
		//printf("Size of grt_interface_info struct is: %lu", sizeof(struct grt_interface_info));
		grt_interfaces[i]->grt_intf_id = i;
	}
	
	for(i=0; i<rte_devcount; i++) {
		struct ether_addr found_dev_macaddr;
		rte_eth_macaddr_get(i, &found_dev_macaddr);
		char ether_addr_string[20]; 
		grt_ethaddr_to_string(ether_addr_string, &found_dev_macaddr);
		printf("  MAC Address found: %s\n", ether_addr_string);
		int j;
		if (conf_ignoremacs == 0) {
		  for (j=0; j < devs_macs_len; j++) {
			  if (strcmp(ether_addr_string, devs_macs[j]) == 0) {
				  devs_port_ids[j] = i;
				  grt_interfaces[j]->rte_port_id = i;
				  memcpy(&grt_interfaces[j]->intf_mac, &found_dev_macaddr, sizeof(struct ether_addr)); //The space is already alloced.
				  //printf("Size of ether_addr struct is: %lu", sizeof(struct ether_addr));
				  printf(" (grt interface id: %i)\n", j);
				  break;
			  }
		  }
		} else {
		  grt_interfaces[i]->rte_port_id = i;
		  devs_port_ids[i] = i;
		  printf(" (grt interface id: %i, 1-to-1 assignment, MACS ignored.)\n", i);
		}
	}

	for (i=0; i < num_devs; i++) {
		if (devs_port_ids[i] == UINT8_UNDEF)
			rte_exit(EXIT_FAILURE, "Specified device (with corresponding MAC address) was not found for interface ID %i.\n", i);
		//Configure each device with one Tx/Rx queue and a default conf.
		status = rte_eth_dev_configure(devs_port_ids[i], 1, 1, &default_ethconf);
		if (status < 0) {
			rte_exit(EXIT_FAILURE, "Could not configure ethernet device %i.\n", i);
		}
	}


	//Set up a mempool for packets


	///unsigned mempool_sz = tz_getMBufMempoolSize(1,1,1,1);	//TODO: maybe make more lcores/ports! This could be carefully calculated
	unsigned mempool_sz = 65536;

	printf("[[I]] Configuring mempool (%u packets per interface)...\n", mempool_sz);

	for (i=0; i < num_devs; i++) {

		//We map interface i's queue (regarding insm's devs_macs index) to lcore i's mempool.
		//The mempools of the other cores (if we have more cores than IFs) remain NULL!

		uint8_t lcore = fwd_available_lcores[i];
		uint8_t socket = rte_lcore_to_socket_id(lcore);
		uint8_t portid = devs_port_ids[i];

		printf("[[I]] Configuring interface %i, lcore=%u, socket=%u, portid=%u", i, lcore, socket, portid);

		char mem_label[64];
		snprintf(mem_label, sizeof(mem_label), "mbuf_pool_%i", i);

		grt_lcore_info[i]->mempool = rte_mempool_create(mem_label,
			mempool_sz, //The number of elements in the mempool. n = (2^q - 1).
			MBUF_SIZE,	//Size of each element
			MEMPOOL_CACHE_SIZE,
			sizeof(struct rte_pktmbuf_pool_private),
			rte_pktmbuf_pool_init, NULL,
			rte_pktmbuf_init, NULL,
			socket, 0);				//TODO: Optimize for NUMA.

		if (grt_lcore_info[i]->mempool == NULL)
			rte_exit(EXIT_FAILURE, "MBuf creation failed for interface %i\n", i);


		char waitbuf_descr[20];
		snprintf(waitbuf_descr, 20, "WAITBUF_RING_%u", i);
		grt_lcore_info[i]->waitbuf = rte_ring_create(waitbuf_descr, WAIT_RING_SIZE, rte_lcore_to_socket_id(lcore), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (grt_lcore_info[i]->waitbuf == NULL) {
			printf("Ring could not be created, errno=%i", rte_errno);
			exit(123);
		}

		printf("[[I]] Preparing queues...\n");

		status = rte_eth_rx_queue_setup(portid, 
			0, 
			RTE_TEST_RX_DESC_DEFAULT,
			socket,
			&rx_conf,
			grt_lcore_info[i]->mempool);

		if (status < 0) rte_exit(EXIT_FAILURE, "Failed to set up interior TX queue\n");
	
		status = rte_eth_tx_queue_setup(portid, 
			0, 
			RTE_TEST_TX_DESC_DEFAULT,
			socket,
			&tx_conf);

		if (status < 0) rte_exit(EXIT_FAILURE, "Failed to set up interior RX queue\n");

		status = rte_eth_dev_start(portid);
		if (status < 0) rte_exit(EXIT_FAILURE, "Failed to fire up interface\n");

		rte_eth_promiscuous_enable(portid);

	}

	if (conf_legacyreplication != 0) {
		const uint8_t socket = 0;
		grt_replication_mempool = rte_mempool_create("mbuf_pool_replication",
			mempool_sz, //The number of elements in the mempool. n = (2^q - 1).
			MBUF_SIZE,	//Size of each element
			MEMPOOL_CACHE_SIZE,
			sizeof(struct rte_pktmbuf_pool_private),
			rte_pktmbuf_pool_init, NULL,
			rte_pktmbuf_init, NULL,
			socket, 0);		
	}

	//Wait for ports up
	printf("[[I]] Waiting for ports up...\n");

	struct rte_eth_link link;
	int allUp = 0;
	while (!allUp) {
		int j;
		for(j=0; j<num_devs; j++) {
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(devs_port_ids[i], &link);
			printf("  Link %i", j);
			if (link.link_status) {
				printf("up\n");
				if (j == num_devs-1) {
					allUp = 1;
					break;
				}
			} else {
				printf("down\n");
				printf(" Still waiting for interface %i\n", j);
				break;
			}
		}
		rte_delay_ms(200);
	}


	int k;
	for (k=0; k < 4; k++) {
		struct grt_interface_info* if_info = grt_getInterfaceInfo(k);
		printf("Testing grt_getInterfaceInfo() %u, %p\n", k, if_info);
	}


	printf("[[I]] Launching data plane cores...\n");
	rte_eal_mp_remote_launch(do_dataplane_job, NULL, CALL_MASTER);
	uint8_t my_lcore;
	RTE_LCORE_FOREACH_SLAVE(my_lcore) {
		if (rte_eal_wait_lcore(my_lcore) < 0)
			return -1;
	}


	printf("[[I]] Go on.\n");


	return 0;

}

struct grt_interface_info* grt_getInterfaceInfo(uint8_t grt_interface_id) {
	return grt_interfaces[grt_interface_id];
}

struct grt_lcore_info_s* grt_getCurrentLCoreInfo() {
	return grt_lcore_info[lcore_to_available_idx[rte_lcore_id()]];
}






