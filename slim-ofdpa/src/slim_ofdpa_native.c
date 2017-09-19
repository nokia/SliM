#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <Python.h>
#include <byteswap.h>

#include "ofdpa_api.h"

#define BUF_PREPEND 16
#define MAX_DATAPATH 16

// Requires: python-dev

#define GE_REWR_DP2VNF 0x10100000 //Base ID for L2 Rewrite group entry from datapath X to VNF Y
#define GE_REWR_VNF2DP 0x10110000 //Base ID for L2 Rewrite group entry from VNF y to datapath X

#define M_STATE_CREATE 0
#define M_STATE_MODIFY 1
#define M_STATE_DELETE 2


int numDP = -1;
uint16_t vlan_base = 0;
uint8_t dp_ext_ports[MAX_DATAPATH];

void hexDump (char *desc, void *addr, int len) {
 //removed: https://stackoverflow.com/questions/7775991/how-to-get-hexdump-of-a-structure-data
}


void checkError(char* context, int rc, int hard) {
	if (rc != OFDPA_E_NONE) {
		printf("OFDPA Error while %s: %i\n", context, rc);
		if (hard != 0) {
			exit(rc);
		}
	}
}

int ofdpa_init() {
	int rc;

	rc = ofdpaClientInitialize("slim_ofdpa_native");
	checkError("ofdpaClientInitialize()", rc, 1);
	return rc;
}

//Receives packets and dumps them in hex notation.
int todo(int argc, char* argv[]) {


	uint8_t ports;
	uint8_t vlansports;

	int rc;

	//If run standalone, do an init here!

	rc = ofdpaClientPktSockBind();
	checkError("pktSockBind()", rc, 1);

	ofdpaPacket_t pkt;
	struct timeval timeout;

	rc = ofdpaMaxPktSizeGet(&pkt.pktData.size);
	checkError("maxPktSizeGet()", rc, 1);

	printf("maxpsize=%i", pkt.pktData.size);

	pkt.pktData.pstart = malloc(pkt.pktData.size+BUF_PREPEND) + BUF_PREPEND;

	while (1) {

		rc = ofdpaPktReceive(NULL, &pkt);
		checkError("receiving packet", rc, 0);
		if (rc == 0) {
			printf("Packet received, size=%i, inPortNum=%i, tableId=%i\n", pkt.pktData.size, pkt.inPortNum, pkt.tableId);
			hexDump ("pkt", pkt.pktData.pstart, pkt.pktData.size);
		}

	}

}



//Injects the drain packet
//If vlan is 0, no output port is used.
int injectDrainPacket(uint16_t vlan, uint8_t port) {

	ofdpa_buffdesc buf;
	uint16_t drainpacket[32];
	memset(&drainpacket, 0, 32);

/*
		SRC_MAC = '\x08\x00\x00\x00\x70\xD0'
		DST_MAC = '\xFF\xFF\xFF\xFF\xFF\xFF'
		ETHTYPE = '\x08\x9a'
		PAYLOAD = '\x57\x47\x30'
*/

	int rc;

	drainpacket[0] = 0xFFFF;
	drainpacket[1] = 0xFFFF;
	drainpacket[2] = 0xFFFF;
	drainpacket[3] = 0x0008;
//	drainpacket[4] = 0x0000
	drainpacket[5] = 0xd070;

	if (vlan == 0) {
		drainpacket[6] = 0x9a08;
		drainpacket[7] = 0x4757;
		drainpacket[8] = 0x0030;
	} else {
		drainpacket[6] = 0x0081;
		drainpacket[7] = __bswap_16(vlan);
		drainpacket[8] = 0x9a08;
		drainpacket[9] = 0x4757;
		drainpacket[10] = 0x0030;
	}

	//rc = ofdpaMaxPktSizeGet(&pkt.pktData.size);
	//checkError("maxPktSizeGet()", rc, 1);
	//printf("maxpsize=%i", pkt.pktData.size);

	buf.pstart = (void*)&drainpacket;
	buf.size = 64;

	printf("Injecting drain packet on VLAN %u, port %u...\n", vlan, port);
	hexDump ("drainPacket", &drainpacket, 64);

	rc = ofdpaPktSend(&buf, 0, port, 1);
	checkError("ofdpaPktSend()", rc, 0);

	return 0;

}

int deleteGroupEntryWithRewrite(uint32_t id, uint8_t destPort, uint16_t newVlan) {

	int rc;

	//Rewrite group

	rc = ofdpaGroupDelete(id);
	checkError("rewr_ge_del", rc, 0);

	//Interface group
	uint32_t intf_gid = 0;
	ofdpaGroupTypeSet(&intf_gid, OFDPA_GROUP_ENTRY_TYPE_L2_INTERFACE);
        ofdpaGroupVlanSet(&intf_gid, newVlan);
        ofdpaGroupPortIdSet(&intf_gid, destPort);

	rc = ofdpaGroupDelete(intf_gid);
	checkError("intf_ge_add", rc, 0);

	return 0;


}

int createGroupEntryWithRewrite(uint32_t id, uint8_t destPort, uint16_t intVlan, uint16_t newVlan, uint8_t popVlan) {

	uint32_t intf_gid = 0;
	ofdpaGroupEntry_t e;
	ofdpaGroupBucketEntry_t bkt;
	int rc;

	//Interface group
	memset(&e, 0, sizeof(ofdpaGroupEntry_t));
	ofdpaGroupTypeSet(&intf_gid, OFDPA_GROUP_ENTRY_TYPE_L2_INTERFACE);
        ofdpaGroupVlanSet(&intf_gid, newVlan);
        ofdpaGroupPortIdSet(&intf_gid, destPort);
	e.groupId = intf_gid;

	memset(&bkt, 0, sizeof(ofdpaGroupBucketEntry_t));	
	bkt.groupId = intf_gid;
    	bkt.bucketIndex = 0;
    	bkt.bucketData.l2UnfilteredInterface.outputPort = destPort;
	//bkt.bucketData.l2UnfilteredInterface.allowVlanTranslation = 1
	bkt.bucketData.l2Interface.popVlanTag = popVlan;



	rc = ofdpaGroupAdd(&e);
	checkError("intf_ge_add", rc, 0);

	rc = ofdpaGroupBucketEntryAdd(&bkt);
	checkError("intf_ge_bkt_add", rc, 0);

	//Rewrite group
	memset(&e, 0, sizeof(ofdpaGroupEntry_t));
	e.groupId = id;

	memset(&bkt, 0, sizeof(ofdpaGroupBucketEntry_t));
	bkt.groupId = id;
	bkt.bucketIndex = 0;
	if (intVlan != newVlan) {
		bkt.bucketData.l2Rewrite.vlanId = OFDPA_VID_PRESENT | newVlan;
	}
	bkt.referenceGroupId = intf_gid;

	rc = ofdpaGroupAdd(&e);
	checkError("rewr_ge_add", rc, 0);

	rc = ofdpaGroupBucketEntryAdd(&bkt);
	checkError("rewr_ge_bkt_add", rc, 0);

	return 0;


}

int addModDPACLFlow(int16_t inPort, uint16_t vlan, uint32_t outGroup, int modify) {

	printf("modDPACLFlow(%i, %i, 0x%x, %i)\n", inPort, vlan, outGroup, modify);

	int rc;

	ofdpaFlowEntry_t flow;
	memset(&flow, 0, sizeof(ofdpaFlowEntry_t));

	rc = ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_ACL_POLICY, &flow);
	checkError("init", rc, 0);

	if (inPort > 0) {
		flow.flowData.policyAclFlowEntry.match_criteria.inPort = inPort;
		flow.flowData.policyAclFlowEntry.match_criteria.inPortMask = OFDPA_INPORT_EXACT_MASK;
	}

	flow.flowData.policyAclFlowEntry.match_criteria.vlanId = OFDPA_VID_PRESENT | vlan;
	flow.flowData.policyAclFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK;

	//DstMac omitted as not needed.

	flow.flowData.policyAclFlowEntry.groupID = outGroup;
	

	if (modify == M_STATE_CREATE) {
		rc = ofdpaFlowAdd(&flow);
	} else if (modify == M_STATE_MODIFY) {
		rc = ofdpaFlowModify(&flow);
	} else {
		rc = ofdpaFlowDelete(&flow);
	}

	checkError("addModACLFlow()", rc, 0);
	
	return 0;

}


int addModDPVLANFlow(int16_t inPort, uint16_t ext_vlan, uint16_t int_vlan, int modify) {

	printf("modDPVlanFlow(%i, %i, %i, %i)\n", inPort, ext_vlan, int_vlan, modify);

	int rc;

	ofdpaFlowEntry_t flow;
	ofdpaFlowEntry_t flow_first;

	if (ext_vlan == 0) {
		memset(&flow_first, 0, sizeof(flow)); 

		rc = ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_VLAN, &flow_first);
		checkError("init", rc, 0);
		flow_first.flowData.vlanFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_TERMINATION_MAC;
		flow_first.flowData.vlanFlowEntry.match_criteria.inPort = inPort;
		flow_first.flowData.vlanFlowEntry.match_criteria.vlanId = OFDPA_VID_PRESENT | int_vlan;
		flow_first.flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK;

	}

	memset(&flow, 0, sizeof(ofdpaFlowEntry_t));

	rc = ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_VLAN, &flow);
	checkError("init", rc, 0);
	flow.flowData.vlanFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_TERMINATION_MAC;
	flow.flowData.vlanFlowEntry.match_criteria.inPort = inPort;
	flow.flowData.vlanFlowEntry.match_criteria.vlanId = ext_vlan == 0?OFDPA_VID_NONE: (OFDPA_VID_PRESENT | ext_vlan);
	flow.flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK;

	if (modify != M_STATE_DELETE && (ext_vlan == 0 || int_vlan != ext_vlan)) {

		flow.flowData.vlanFlowEntry.setVlanIdAction = 1;
		flow.flowData.vlanFlowEntry.newVlanId = OFDPA_VID_PRESENT | int_vlan;

	}
	

	if (modify == M_STATE_CREATE) {
		if (ext_vlan == 0) {
			rc = ofdpaFlowAdd(&flow_first);
			checkError("addVLANFlow_first", rc, 0);
		}
		rc = ofdpaFlowAdd(&flow);
		checkError("addVLANFlow", rc, 0);
	} else if (modify == M_STATE_MODIFY) {
		rc = ofdpaFlowModify(&flow);
		//TODO!
	} else {
		rc = ofdpaFlowDelete(&flow);
		checkError("deleteVLANFlow", rc, 0);
		if (ext_vlan == 0) {
			rc = ofdpaFlowDelete(&flow_first);
			checkError("deleteVLANFlow_first", rc, 0);
		}
	}

	return 0;


}

//Or deinit if reverse is set to != 0
int initPipeDatapath(uint8_t dpId, uint8_t vnfId, uint16_t vlan_shift, uint8_t workloadPort, uint8_t vnfPort, int reverse) {
	
	uint16_t dpVlan = vlan_base + dpId + 1; //+1 as the first is the management vlan

	uint32_t group2Vnf = GE_REWR_DP2VNF | dpId | (vnfId << 8);
	uint32_t group2DP = GE_REWR_VNF2DP | dpId;

	//Assumes group entries are set: L2 Rewrite/Interface Group entry.
	//vlanOffset includes a potential shift!

	if (reverse == 0) {

		//Direction Workload -> VNF
		createGroupEntryWithRewrite(group2Vnf, vnfPort, dpVlan, dpVlan+vlan_shift, 0);
		addModDPVLANFlow(workloadPort, 0, dpVlan, M_STATE_CREATE);
		addModDPACLFlow(workloadPort, dpVlan, group2Vnf, M_STATE_CREATE);	//Note: Group entry must shift the VLAN here!

		//Direction VNF -> Workload
		addModDPVLANFlow(vnfPort, dpVlan+vlan_shift, dpVlan, M_STATE_CREATE);
		addModDPACLFlow(vnfPort, dpVlan, group2DP, M_STATE_CREATE);

	} else {

		//Reverse order for deletion:

		//Direction Workload -> VNF
		addModDPACLFlow(workloadPort, dpVlan, group2Vnf, M_STATE_DELETE);	//Note: Group entry must shift the VLAN here!
		addModDPVLANFlow(workloadPort, 0, dpVlan, M_STATE_DELETE);
		deleteGroupEntryWithRewrite(group2Vnf, vnfPort, dpVlan+vlan_shift);

		//Direction VNF -> Workload
		addModDPACLFlow(vnfPort, dpVlan, group2DP, M_STATE_DELETE);
		addModDPVLANFlow(vnfPort, dpVlan+vlan_shift, dpVlan, M_STATE_DELETE);

	}

	return 0;

}

//Stage 0 prepares datapath for migration, except the final switchover. Creates the complete way VNF > Datapath, 
//and prepares group tables for Datapath > VNF.
//Stage 0 does the final switchover. The drain packet is NOT injected by this function!
int migrateRaw(uint8_t dpId, uint8_t old_vnfId, uint8_t vnfId, uint16_t old_vlan_shift, uint16_t vlan_shift, 
	uint8_t workloadPort, uint8_t old_vnfPort, uint8_t vnfPort, int reverse, uint8_t stage) {
	
	uint16_t dpVlan = vlan_base + dpId + 1; //+1 as the first is the management vlan

	uint32_t old_group2Vnf = GE_REWR_DP2VNF | dpId | (old_vnfId << 8);
	uint32_t group2Vnf = GE_REWR_DP2VNF | dpId | (vnfId << 8);
	uint32_t group2DP = GE_REWR_VNF2DP | dpId;

	//Assumes group entries are set: L2 Rewrite/Interface Group entry.
	//vlanOffset includes a potential shift!


	if (stage == 0) {

		if (reverse == 0) {

			//Direction Workload -> VNF
			createGroupEntryWithRewrite(group2Vnf, vnfPort, dpVlan, dpVlan+vlan_shift, 0);
			//addModDPVLANFlow(workloadPort, 0, dpVlan, M_STATE_CREATE); 	//Exists from previous init.
			//addModDPACLFlow(workloadPort, dpVlan, group2Vnf, M_STATE_CREATE);	//Created in stage 2

			//Direction VNF -> Workload
			addModDPVLANFlow(vnfPort, dpVlan+vlan_shift, dpVlan, M_STATE_CREATE);	//This direction can be completely created.
			if (old_vnfPort != vnfPort) {
				//Otherwise, we can use the same ACL flow for the old and the new VNF. 
				//The vlan shift is then done by the VLAN flow table.
				addModDPACLFlow(vnfPort, dpVlan, group2DP, M_STATE_CREATE);
			}

		} else {

			//Reverse order for deletion:

			//Direction Workload -> VNF
			//addModDPACLFlow(workloadPort, dpVlan, group2Vnf, M_STATE_DELETE);	//Created in stage 2
			//addModDPVLANFlow(workloadPort, 0, dpVlan, M_STATE_DELETE);		//Should be kept for original 
												//flow to operate.
			deleteGroupEntryWithRewrite(group2Vnf, vnfPort, dpVlan+vlan_shift);

			//Direction VNF -> Workload
			if (old_vnfPort != vnfPort) {
				//Otherwise, we can use the same ACL flow for the old and the new VNF. 
				//The vlan shift is then done by the VLAN flow table.
				addModDPACLFlow(vnfPort, dpVlan, group2DP, M_STATE_DELETE);
			}
			addModDPVLANFlow(vnfPort, dpVlan+vlan_shift, dpVlan, M_STATE_DELETE);

		}

	} else if (stage == 1) {

		if (reverse == 0) {

			//Direction Workload -> VNF
			addModDPACLFlow(workloadPort, dpVlan, group2Vnf, M_STATE_MODIFY);


		} else {

			printf("Error: Stage 1 cannot be reversed as it is final, cleanup (Stage 2) next to have a clean dest VM state.");
		}

	} else if (stage == 2) {	//Fake stage, called by cleanup.

		if (reverse == 0) {

			//Direction Workload -> VNF
			deleteGroupEntryWithRewrite(old_group2Vnf, old_vnfPort, dpVlan+old_vlan_shift);

			//Direction VNF -> Workload
			if (old_vnfPort != vnfPort) {
				addModDPACLFlow(old_vnfPort, dpVlan, group2DP, M_STATE_DELETE);
			}
			addModDPVLANFlow(old_vnfPort, dpVlan+old_vlan_shift, dpVlan, M_STATE_DELETE);


		} else {

			printf("Error: Stage 2 cannot be reversed as it is final.");
		}

	}

	//TODO: Here we now have 1 stale group entry Workload > VNF, and a complete stale way back.

	return 0;

}


int dpinit(uint8_t vnfId, uint16_t vlan_shift, uint8_t vnfPort, int reverse) {
	int i;	
	for (i=0; i < numDP; i++) {
		initPipeDatapath(i, vnfId, vlan_shift, dp_ext_ports[i], vnfPort, reverse);
	}

	return 0;
}

int dpmigrate(uint8_t old_vnfId, uint8_t vnfId, uint16_t old_vlan_shift, uint16_t vlan_shift, uint8_t old_vnfPort, uint8_t vnfPort, int reverse, uint8_t stage) {
	int i;

	if (stage < 0 || stage > 2) {
		printf("Illegal stage, exiting\n");
		return -1;
	}

	for (i=0; i < numDP; i++) {
		migrateRaw(i, old_vnfId, vnfId, old_vlan_shift, vlan_shift, dp_ext_ports[i], old_vnfPort, vnfPort, reverse, stage);
	}

	if (stage == 1) {
		for (i=0; i < numDP; i++) {
			injectDrainPacket(vlan_base+old_vlan_shift+i+1, old_vnfPort);
		}
	}

	return 0;

}



// =========================== Python wrappers ===================


//Execute this when a VNF is inited
extern PyObject* py_vnf_dpinit(PyObject* self, PyObject* args) {

	uint8_t vnfId;
	uint16_t vlan_shift;
	uint8_t vnfPort;
	int reverse;

	PyArg_ParseTuple(args, "BHBi", &vnfId, &vlan_shift, &vnfPort, &reverse);

	printf("vnf_dpinit(vnfId=%u, vlan_shift=%u, vnfPort=%u, reverse=%u)\n", vnfId, vlan_shift, vnfPort, reverse);

	dpinit(vnfId, vlan_shift, vnfPort, reverse);

	printf("DP init finished.\n");

  	return Py_BuildValue("i", 0);

}

//Execute this when a VNF is migrated
extern PyObject* py_vnf_dpmigrate(PyObject* self, PyObject* args) {

	uint8_t old_vnfId;
	uint8_t vnfId;
	uint16_t old_vlan_shift;
	uint16_t vlan_shift;
	uint8_t old_vnfPort;
	uint8_t vnfPort;
	uint8_t reverse;
	uint8_t stage;

	PyArg_ParseTuple(args, "BBHHBBBB", &old_vnfId, &vnfId, &old_vlan_shift, &vlan_shift, &old_vnfPort, &vnfPort, &reverse, &stage);

	printf("vnf_dpmigrate(old_vnfId=%u, vnfId=%u, old_vlan_shift=%u, vlan_shift=%u, old_vnfPort=%u, vnfPort=%u, reverse=%u, stage=%u)\n", old_vnfId, vnfId, old_vlan_shift, vlan_shift, old_vnfPort, vnfPort, reverse, stage);

	dpmigrate(old_vnfId, vnfId, old_vlan_shift, vlan_shift, old_vnfPort, vnfPort, reverse, stage);

	printf("DP migrate finished.\n");

	int ret = 0;
  	return Py_BuildValue("i", ret);

}

//Execute this at the beginning for every datapath (except mgmt)
extern PyObject* py_init_dp_ext(PyObject* self, PyObject* args) {

	uint8_t dpid;
	uint16_t dp_ext_port;

	PyArg_ParseTuple(args, "BH", &dpid, &dp_ext_port);

	uint16_t dpVlan = vlan_base + dpid + 1; //+1 as the first is the management vlan
	dp_ext_ports[dpid] = dp_ext_port;
	uint32_t group2DP = GE_REWR_VNF2DP | dpid;
	createGroupEntryWithRewrite(group2DP, dp_ext_port, dpVlan, dpVlan, 1);

	printf("Native: Datapath %i (dp_ext_port=%i) initialized successfully\n", dpid, dp_ext_port);

	int ret = 0;
  	return Py_BuildValue("i", ret);

}

//Execute this at the beginning
extern PyObject* py_init(PyObject* self, PyObject* args) {

	char* testchar;

	PyArg_ParseTuple(args, "siH", &testchar, &numDP, &vlan_base);

	//ofdpa_init(); //not necessary, done in Python.

	printf("Native classes initialized successfully, OFDPA inited. %s, %i\n", testchar, numDP);

	char *s = "Hello from C!";
  	return Py_BuildValue("s", s);

}

static PyMethodDef pymethods[] = {
  {"init", py_init, METH_VARARGS},
  {"vnf_dpinit", py_vnf_dpinit, METH_VARARGS},
  {"vnf_dpmigrate", py_vnf_dpmigrate, METH_VARARGS},
  {"init_dp_ext", py_init_dp_ext, METH_VARARGS},
//  {"myOtherFunction", py_myOtherFunction, METH_VARARGS},
  {NULL, NULL}
};


extern void initslim_ofdpa_native (void) {
	(void) Py_InitModule("slim_ofdpa_native", pymethods);
}







