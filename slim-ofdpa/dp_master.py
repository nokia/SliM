
from deps.OFDPA_python import *
from socket import *
import slim_ofdpa_native
import time

l2group_id_ctr = 0

def ofdpa_init(conf):
	status = ofdpaClientInitialize("OFDPA_SliM")
	if (status == OFDPA_E_NONE):
		return 0
	else:
		print "OFDPA returned error code " + str(status)
		return -1

def init_mgmt(conf):

	mgmt_vlan = conf["vlan_base"]

	if_ext = conf["ext"]["mgmt"]
	if_ext_mgmt_vlan = setupIntfGroup(if_ext, mgmt_vlan, False)
	setupVlanFlowEntry(if_ext, 0, mgmt_vlan)
	for mac in conf["ext"]["mgmt_macs"]:
		makePolicyACLEntry(False, mgmt_vlan, mac, False, False, False, if_ext_mgmt_vlan)

	for vm,data in conf["vms"].iteritems():
		my_mgmtVlan = mgmt_vlan+data["vlan_shift"]
		shifted = data["vlan_shift"] != 0

		iface = data["port"]
		iface_mgmt_vlan = setupIntfGroup(iface, my_mgmtVlan, True)
		setupVlanFlowEntry(iface, my_mgmtVlan, mgmt_vlan)
		for mac in data["mgmt_macs"]:
			makePolicyACLEntry(False, mgmt_vlan, mac, False, False, my_mgmtVlan if shifted else False, iface_mgmt_vlan)



#Inits a specific VNF for forwarding in the datapath
def dp_vnf_init(conf, vmName):	

	data = conf["vms"][vmName]
	vmId = conf["vms"][vmName]["id"]

	port = data["port"]
	vlan_shift = data["vlan_shift"]

	#TODO: Strict checking for parameter types to avoid stack overflows in C!
	print "From  python vnf_dpinit: ", vmId, vlan_shift, port, 0
	slim_ofdpa_native.vnf_dpinit(vmId, vlan_shift, port, 0)



#Deinits a specific VNF for forwarding in the datapath
def dp_vnf_deinit(conf, vmName):	

	data = conf["vms"][vmName]
	vmId = conf["vms"][vmName]["id"]

	port = data["port"]
	vlan_shift = data["vlan_shift"]

	print "From  python vnf_dpinit: ", vmId, vlan_shift, port, 1
	slim_ofdpa_native.vnf_dpinit(vmId, vlan_shift, port, 1)


def dp_vnf_migrate(conf, srcVM, destVM, stage):
	
	old_data = conf["vms"][srcVM]
	data = conf["vms"][destVM]

	old_port = old_data["port"]
	old_vmId = old_data["id"]
	old_vlan_shift = old_data["vlan_shift"]
	port = data["port"]
	vmId = data["id"]
	vlan_shift = data["vlan_shift"]

	#TODO: Strict checking for parameter types to avoid stack overflows in C!
	print "From  python vnf_dpmigrate: ", old_vmId, vmId, old_vlan_shift, vlan_shift, old_port, port, 0, stage
	slim_ofdpa_native.vnf_dpmigrate(old_vmId, vmId, old_vlan_shift, vlan_shift, old_port, port, 0, stage)



#Inits every datapath at the beginning
def init_dp(conf):
	i = 0
	for dp_extport in conf["ext"]["dp_ports"]:
		slim_ofdpa_native.init_dp_ext(i, dp_extport);

		i += 1

	print "Finished initing datapath."



#Inits everything (called from ext)
def init(conf):

	slim_ofdpa_native.init("Test", 2, conf["vlan_base"])

	ofdpa_init(conf)
	init_mgmt(conf)
	init_dp(conf)

	#time.sleep(3)
	#dp_vnf_init(conf, "vm0")	#TODO: triggered by REST interface
	#time.sleep(5)
	#//dp_vnf_deinit(conf, "vm0")	#TODO: triggered by REST interface
	#time.sleep(3)
	#dp_vnf_init(conf, "vm1")	#TODO: triggered by REST interface
	#time.sleep(5)
	#dp_vnf_deinit(conf, "vm1")	#TODO: triggered by REST interface


errors = {
	 0 : "OFDPA_E_NONE",
	 -20 : "OFDPA_E_RPC (Error in RPC.)",
	 -21 : "OFDPA_E_INTERNAL (Internal error.)",
	 -22 : "OFDPA_E_PARAM (Invalid parameter.)",
	 -23 : "OFDPA_E_ERROR (Parameter constraint violated.)",
	 -24 : "OFDPA_E_FULL (Maximum count is already reached or table full.)",
	 -25 : "OFDPA_E_EXISTS (Already exists.)",
	 -26 : "OFDPA_E_TIMEOUT (Operation Timeout.)",
	 -27 : "OFDPA_E_FAIL",
	 -28 : "OFDPA_E_DISABLED",
	 -29 : "OFDPA_E_UNAVAIL",
	 -30 : "OFDPA_E_NOT_FOUND",
	 -31 : "OFDPA_E_EMPTY",
	 -32 : "OFDPA_E_REQUEST_DENIED",
	 -33 : "OFDPA_NOT_IMPLEMENTED_YET",
}

def decerr(rc):
	rc_str = errors.get(rc)
	if rc_str == None :
		return "Unknown (" + str(rc) + ")"
	return rc_str;


def makePolicyACLEntryCtrl(inPort, vlan, dstMac):
	print "Destination MAC", dstMac
	global l2group_id_ctr
        entry = ofdpaFlowEntry_t()
        ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_ACL_POLICY, entry)
	mc = entry.flowData.policyAclFlowEntry.match_criteria
	if inPort:
		mc.inPort = inport
		mc.inPortMask = (OFDPA_INPORT_EXACT_MASK)
	#mc.etherTypeMask = (OFDPA_ETHERTYPE_ALL_MASK)
	mc.vlanId = OFDPA_VID_PRESENT | vlan
	mc.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK
	if dstMac:
		MACAddress_set(mc.destMac, str(dstMac))
		MACAddress_set(mc.destMacMask, "ff:ff:ff:ff:ff:ff")

        entry.flowData.policyAclFlowEntry.outputPort = OFDPA_PORT_CONTROLLER


        rc = ofdpaFlowAdd(entry)
        if rc != OFDPA_E_NONE:
          print "Error from ACL policy table ofdpaFlowAdd(). rc = ", decerr(rc)




def makePolicyACLEntry(inPort, vlan, dstMac, newDstMac, newSrcMac, newVlanId, targetGroup):
	print "Destination MAC", dstMac
	global l2group_id_ctr
        entry = ofdpaFlowEntry_t()
        ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_ACL_POLICY, entry)
	mc = entry.flowData.policyAclFlowEntry.match_criteria
	if inPort:
		mc.inPort = inport
		mc.inPortMask = (OFDPA_INPORT_EXACT_MASK)
	#mc.etherTypeMask = (OFDPA_ETHERTYPE_ALL_MASK)
	mc.vlanId = OFDPA_VID_PRESENT | vlan
	mc.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK
	if dstMac:
		MACAddress_set(mc.destMac, str(dstMac))
		MACAddress_set(mc.destMacMask, "ff:ff:ff:ff:ff:ff")

        #entry.flowData.policyAclFlowEntry.outputPort = OFDPA_PORT_CONTROLLER

	if (newSrcMac or newDstMac or newVlanId):
		l2group_id_ctr += 1

		groupid = new_uint32_tp()
		group = ofdpaGroupEntry_t()
		uint32_tp_assign(groupid, 0x10000000 | l2group_id_ctr) #L2 Rewrite Entry
		group.groupId = uint32_tp_value(groupid)

		bkt = ofdpaGroupBucketEntry_t()
		bkt.groupId = group.groupId
		bkt.bucketIndex = 0
		
		rewr = bkt.bucketData.l2Rewrite
		if newSrcMac:
			MACAddress_set(rewr.srcMac, newSrcMac)
		if newDstMac:
			MACAddress_set(rewr.dstMac, newDstMac)
		if newVlanId:
			rewr.vlanId = OFDPA_VID_PRESENT | newVlanId

		bkt.referenceGroupId = targetGroup

		rc = ofdpaGroupAdd(group)
		if rc != OFDPA_E_NONE:
		  print "Error from ofdpaGroupAdd(). rc = ", decerr(rc)
		  return #do not install the bucket.

		rc = ofdpaGroupBucketEntryAdd(bkt)
		if rc != OFDPA_E_NONE:
		  print "Error from ofdpaGroupBucketAdd(). rc = ", decerr(rc)

		entry.flowData.policyAclFlowEntry.groupID = bkt.groupId
	else:
		entry.flowData.policyAclFlowEntry.groupID = targetGroup

        rc = ofdpaFlowAdd(entry)
        if rc != OFDPA_E_NONE:
          print "Error from ACL policy table ofdpaFlowAdd(). rc = ", decerr(rc)


def setupVlanFlowEntry(inport, ext_vlan, int_vlan):
	
	print "Setting up VLAN flow entry " + str(inport) + ", " + str(ext_vlan) + ", " + str(int_vlan)

	untagged = True if ext_vlan == 0 else False

	if (untagged):
		vlan_o = ofdpaFlowEntry_t()
		ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_VLAN, vlan_o)
		vlan_o.flowData.vlanFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_TERMINATION_MAC
		vlan_o.flowData.vlanFlowEntry.match_criteria.inPort = inport
		vlan_o.flowData.vlanFlowEntry.match_criteria.vlanId = OFDPA_VID_PRESENT | int_vlan
		vlan_o.flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK


		rc = ofdpaFlowAdd(vlan_o)
		if rc != OFDPA_E_NONE:
		  print "Error from inner ofdpaFlowAdd(). rc = ", decerr(rc)

	vlan_e = ofdpaFlowEntry_t()
	ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_VLAN, vlan_e)
	vlan_e.flowData.vlanFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_TERMINATION_MAC
	vlan_e.flowData.vlanFlowEntry.match_criteria.inPort = inport
	vlan_e.flowData.vlanFlowEntry.match_criteria.vlanId = OFDPA_VID_NONE if untagged else (OFDPA_VID_PRESENT | ext_vlan)
	vlan_e.flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK 

	if (untagged or int_vlan != ext_vlan):
        	vlan_e.flowData.vlanFlowEntry.setVlanIdAction = 1
		vlan_e.flowData.vlanFlowEntry.newVlanId = OFDPA_VID_PRESENT | int_vlan

	rc = ofdpaFlowAdd(vlan_e)
        if rc != OFDPA_E_NONE:
          print "Error from outer ofdpaFlowAdd(). rc = ", decerr(rc)



def setupIntfGroup(port, vlan, tagged):

	print "> Installing unfiltered group entry for interface ", port

	groupid = new_uint32_tp()
	entry = ofdpaGroupEntry_t()
	ofdpaGroupTypeSet(groupid, OFDPA_GROUP_ENTRY_TYPE_L2_INTERFACE)
        ofdpaGroupVlanSet(groupid, vlan)
        ofdpaGroupPortIdSet(groupid, port)
        entry.groupId = uint32_tp_value(groupid)

	bkt = ofdpaGroupBucketEntry_t()
	bkt.groupId = entry.groupId
    	bkt.bucketIndex = 0
    	bkt.bucketData.l2UnfilteredInterface.outputPort = port
	bkt.bucketData.l2UnfilteredInterface.allowVlanTranslation = 1
	bkt.bucketData.l2Interface.popVlanTag = (0 if tagged else 1)

	rc = ofdpaGroupAdd(entry)
        if rc != OFDPA_E_NONE:
          print "Error from ofdpaGroupAdd(). rc = ", decerr(rc)
	  return #do not install the bucket.

	rc = ofdpaGroupBucketEntryAdd(bkt)
        if rc != OFDPA_E_NONE:
          print "Error from ofdpaGroupBucketAdd(). rc = ", decerr(rc)

	print hex(entry.groupId)

	return entry.groupId



# Not used yet. Does not correctly work for stripping VLAN tags!
def setupEgress(vlan, port, ext_vlan):

	print "> Installing egress functionality", vlan, port, ext_vlan

	if ext_vlan != vlan:
		egr = ofdpaFlowEntry_t()
		ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_EGRESS_VLAN, egr)
		#TODO
		egr.flowData.egressVlanFlowEntry.match_criteria.outPort = port
		egr.flowData.egressVlanFlowEntry.match_criteria.vlanId = OFDPA_VID_PRESENT | vlan
		egr.flowData.egressVlanFlowEntry.match_criteria.allowVlanTranslation = 1
		if (ext_vlan != 0):
			egr.flowData.egressVlanFlowEntry.setVlanIdAction = 1
			egr.flowData.egressVlanFlowEntry.newVlanId = OFDPA_VID_PRESENT | ext_vlan
		else:
			print "Untagged.."
			#egr.flowData.egressVlanFlowEntry.ovidAction = 1
			#egr.flowData.egressVlanFlowEntry.ovid = OFDPA_VID_NONE
			#egr.flowData.egressVlanFlowEntry.popVlanAction = 1
			#egr.flowData.egressVlanFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_EGRESS_VLAN_1
			egr.flowData.egressVlanFlowEntry.setVlanIdAction = 1
			#egr.flowData.egressVlanFlowEntry.newVlanId = OFDPA_VID_PRESENT | 1
			egr.flowData.egressVlanFlowEntry.newVlanId = OFDPA_VID_NONE

		rc = ofdpaFlowAdd(egr)
		if rc != OFDPA_E_NONE:
		  print "Error from egresstable ofdpaFlowAdd(). rc = ", decerr(rc)


def setBridgeEntry(vlanid, macaddress, target_groupid):
	print "setBridgeEntry", vlanid, macaddress, target_groupid

        br_e = ofdpaFlowEntry_t()
        ofdpaFlowEntryInit(OFDPA_FLOW_TABLE_ID_BRIDGING, br_e)
        br_e.flowData.bridgingFlowEntry.gotoTableId = OFDPA_FLOW_TABLE_ID_ACL_POLICY
        br_e.flowData.bridgingFlowEntry.groupID = target_groupid
        br_e.flowData.bridgingFlowEntry.match_criteria.vlanId = (OFDPA_VID_PRESENT | vlanid)
        br_e.flowData.bridgingFlowEntry.match_criteria.vlanIdMask = (OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK)
        MACAddress_set(br_e.flowData.bridgingFlowEntry.match_criteria.destMac, macaddress)
        MACAddress_set(br_e.flowData.bridgingFlowEntry.match_criteria.destMacMask, "ff:ff:ff:ff:ff:ff")

        rc = ofdpaFlowAdd(br_e)
        if rc != OFDPA_E_NONE:
          print "Error from bridgetable ofdpaFlowAdd(). rc = ", decerr(rc)














