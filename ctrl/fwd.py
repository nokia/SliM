# Ryu app for OpenFlow-based traffic control. Currently only supports one datapath.

import logging
import numbers
import socket
import struct

import json
from webob import Response

from ryu.app.wsgi import ControllerBase
from ryu.app.wsgi import WSGIApplication
from ryu.base import app_manager
from ryu.controller import dpset
from ryu.controller import ofp_event
from ryu.controller.handler import set_ev_cls
from ryu.controller.handler import MAIN_DISPATCHER
from ryu.controller.handler import CONFIG_DISPATCHER
from ryu.exception import OFPUnknownVersion
from ryu.exception import RyuException
from ryu.lib import dpid as dpid_lib
from ryu.lib import hub
from ryu.lib import mac as mac_lib
from ryu.lib import addrconv
from ryu.lib.packet import arp
from ryu.lib.packet import ethernet
from ryu.lib.packet import icmp
from ryu.lib.packet import ipv4
from ryu.lib.packet import packet
from ryu.lib.packet import vlan
from ryu.lib.packet import ether_types
from ryu.ofproto import ether
from ryu.ofproto import inet
from ryu.ofproto import ofproto_v1_0

import pprint

from topo import grt_topo

PATTERN_ALLTEXT = r'.*'
PATTERN_STAGE = r'[01]'

pp = pprint.PrettyPrinter(indent=4)

class GRTFwd(app_manager.RyuApp):
	OFP_VERSIONS = [ofproto_v1_0.OFP_VERSION]

	_CONTEXTS = {'dpset': dpset.DPSet,	#TODO: DPSet needed?
		'wsgi': WSGIApplication}

    	def __init__(self, *args, **kwargs):
		super(GRTFwd, self).__init__(*args, **kwargs)
	
		#GRTFwdCtrl.set_logger(self.logger)

		wsgi = kwargs['wsgi']
		self.waiters = {}
		self.data = {'waiters': self.waiters}

		mapper = wsgi.mapper

		path = '/grt/migrate/{source_vm}/{dest_vm}/{stage}'
		mapper.connect('grt', path, controller=GRTFwdCtrl,
				requirements={'source_vm':PATTERN_ALLTEXT, 'dest_vm':PATTERN_ALLTEXT},
				action='migrate',
				conditions=dict(method=['POST']))

		path = '/grt/init/{init_vm}'
		mapper.connect('grt', path, controller=GRTFwdCtrl,
				requirements={'init_vm':PATTERN_ALLTEXT},
				action='grtinit',
				conditions=dict(method=['POST']))

		path = '/grt/reinit'
		mapper.connect('grt', path, controller=GRTFwdCtrl,
				action='reinit',
				conditions=dict(method=['POST']))

		path = '/grt/deinit/{init_vm}'
		mapper.connect('grt', path, controller=GRTFwdCtrl,
				requirements={'init_vm':PATTERN_ALLTEXT},
				action='deinit',
				conditions=dict(method=['POST']))

		path = '/grt/cleanupOld'
		mapper.connect('grt', path, controller=GRTFwdCtrl,
				action='cleanupOld',
				conditions=dict(method=['POST']))

		dpset = kwargs['dpset']

		pp.pprint(dpset.get_all())


	@set_ev_cls(ofp_event.EventOFPPacketIn, MAIN_DISPATCHER)
	def _packet_in_handler(self, ev):
		msg = ev.msg
		datapath = msg.datapath
		ofproto = datapath.ofproto

		pkt = packet.Packet(msg.data)
		eth = pkt.get_protocol(ethernet.ethernet)

		if eth.ethertype == ether_types.ETH_TYPE_LLDP:
		    # ignore lldp packet
		    return
		dst = eth.dst
		src = eth.src
		ethtype = eth.ethertype

		mgmt_vlan_id = grt_topo["misc"]["mgmt_vlan"]

		dpid = datapath.id

		self.logger.info("packet in %s %s %s %s %x", dpid, src, dst, msg.in_port, ethtype)




	@set_ev_cls(ofp_event.EventOFPPortStatus, MAIN_DISPATCHER)
	def _port_status_handler(self, ev):
		msg = ev.msg
		reason = msg.reason
		port_no = msg.desc.port_no

		ofproto = msg.datapath.ofproto
		if reason == ofproto.OFPPR_ADD:
		    self.logger.info("port added %s", port_no)
		elif reason == ofproto.OFPPR_DELETE:
		    self.logger.info("port deleted %s", port_no)
		elif reason == ofproto.OFPPR_MODIFY:
		    self.logger.info("port modified %s", port_no)
		else:
		    self.logger.info("Illeagal port state %s %s", port_no, reason)


	@set_ev_cls(ofp_event.EventOFPSwitchFeatures, CONFIG_DISPATCHER)
	def switch_features_handler(self, ev):
		dp = ev.msg.datapath
		if dp.id == grt_topo['misc']['datapath']:
			self.logger.info("datapath added %u", dp.id)
			GRTFwdCtrl.set_datapath(dp)
		else:
			self.logger.warn("datapath found %u, but not our datapath to use", dp.id)

		#Do this below, to be able to get a learning switch running.
				
		ofproto = dp.ofproto
		#match = dp.ofproto_parser.OFPMatch()
		#mod = dp.ofproto_parser.OFPFlowMod(
		#	datapath=dp, match=match, cookie=0,
		#	command=ofproto.OFPFC_DELETE,
		#	priority=ofproto.OFP_DEFAULT_PRIORITY,
		#	flags=ofproto.OFPFF_SEND_FLOW_REM)
		#dp.send_msg(mod)

		mgmt_vlan = grt_topo["misc"]["mgmt_vlan"]

		parser = dp.ofproto_parser

		#match = parser.OFPMatch(dl_vlan=mgmt_vlan)

		#actions = [parser.OFPActionOutput(ofproto.OFPP_CONTROLLER)]

		#mod = dp.ofproto_parser.OFPFlowMod(
		#	datapath=dp, match=match, cookie=123,
		#	command=ofproto.OFPFC_MODIFY, idle_timeout=0, hard_timeout=0,
		#	priority=ofproto.OFP_DEFAULT_PRIORITY-100,
		#	flags=ofproto.OFPFF_SEND_FLOW_REM, actions=actions)

		#dp.send_msg(mod)

		match = parser.OFPMatch(dl_vlan=mgmt_vlan)

		actions = [parser.OFPActionOutput(ofproto.OFPP_NORMAL)]

		mod = dp.ofproto_parser.OFPFlowMod(
			datapath=dp, match=match, cookie=123,
			command=ofproto.OFPFC_MODIFY, idle_timeout=0, hard_timeout=0,
			priority=ofproto.OFP_DEFAULT_PRIORITY+100,
			flags=ofproto.OFPFF_SEND_FLOW_REM, actions=actions)

		dp.send_msg(mod)

	@set_ev_cls(ofp_event.EventOFPBarrierReply, MAIN_DISPATCHER)
	def barrier_request_handler(self, ev):
		#It is now guaranteed that all queues have switched over...
		for outPort in GRTFwdCtrl.targetsForDrainPacket:
			dp=ev.msg.datapath
			ofproto = dp.ofproto

			actions = [dp.ofproto_parser.OFPActionVlanVid(outPort["vlan"]), dp.ofproto_parser.OFPActionOutput(ofproto.OFPP_FLOOD)]
#, 
#outPort["openflow_portid"]

			packet=GRTFwdCtrl.getNewDrainPacket();

			out = dp.ofproto_parser.OFPPacketOut(
			    datapath=dp, buffer_id=0xffffffff, in_port=ofproto.OFPP_CONTROLLER,
			    actions=actions, data=packet)
			print "Barrier response received, sending drain packet out on " + str(outPort) + "..."
			dp.send_msg(out)
		
		GRTFwdCtrl.targetsForDrainPacket = []


class GRTFwdCtrl(ControllerBase):
	

	@classmethod
	def set_datapath(cls, dp):
		print "Setting datapath"
		cls.grt_dp = dp
		cls.setVM = None
		cls.nextVM = None
		cls.fromVmPaths2Cleanup = set()
		cls.targetsForDrainPacket = []

	@classmethod
	def getNewDrainPacket(cls):

		SRC_MAC = '\x08\x00\x00\x00\x70\xD0'
		DST_MAC = '\xFF\xFF\xFF\xFF\xFF\xFF'
		ETHTYPE = '\x08\x9a'
		PAYLOAD = '\x57\x47\x30'


		drainpacket = bytearray(DST_MAC + SRC_MAC + ETHTYPE + PAYLOAD)

		for i in range(43):		# Pad to minimum transmission unit
			drainpacket = drainpacket + '\x00'

		return drainpacket


	def grtinit(self, req, init_vm, **_kwargs):
		print "INIT"
		print("Shall init " + init_vm + " on " + str(self.grt_dp.id))

		num_ports = grt_topo["misc"]["num_ports"]

		if GRTFwdCtrl.setVM:
			self.fail(req, "Cannot init if another VM was inited before.")
			return

		for i in range(num_ports):

			grt_vm_port = grt_topo[init_vm]["ports"][i]
			grt_perimeter_port = grt_topo["misc"]["perimeter_ports"][i]

			self.setFlow(grt_vm_port, grt_perimeter_port, 0)
			self.setFlow(grt_perimeter_port, grt_vm_port, 0)

		GRTFwdCtrl.setVM = init_vm

	def deinit(self, req, init_vm, **_kwargs):
		print "INIT"
		print("Shall deinit " + init_vm + " on " + str(self.grt_dp.id))

		num_ports = grt_topo["misc"]["num_ports"]

		if GRTFwdCtrl.setVM != init_vm:
			self.fail(req, "Shall deinit " + init_vm
			 + ", but " + ("none" if not GRTFwdCtrl.setVM else GRTFwdCtrl.setVM)  + " is active. No need to do something.")
			return

		for i in range(num_ports):

			grt_vm_port = grt_topo[init_vm]["ports"][i]
			grt_perimeter_port = grt_topo["misc"]["perimeter_ports"][i]

			self.unsetFlow(grt_vm_port)
			self.unsetFlow(grt_perimeter_port)

		GRTFwdCtrl.setVM = None
		

	def cleanupOld(self, req, **_kwargs):

		num_ports = grt_topo["misc"]["num_ports"]

		for vm2Clup in GRTFwdCtrl.fromVmPaths2Cleanup:
			for i in range(num_ports):
				grt_vm_port = grt_topo[vm2Clup]["ports"][i]
				#grt_perimeter_port = grt_topo["misc"]["perimeter_ports"][i]
				self.unsetFlow(grt_vm_port)

		GRTFwdCtrl.fromVmPaths2Cleanup = set()
	


	def reinit(self, req, **_kwargs):
		
		dp=self.grt_dp
		ofproto = dp.ofproto

		match = dp.ofproto_parser.OFPMatch()

		mod = dp.ofproto_parser.OFPFlowMod(
			datapath=dp, match=match, cookie=0,
			command=ofproto.OFPFC_DELETE,
			priority=ofproto.OFP_DEFAULT_PRIORITY,
			flags=ofproto.OFPFF_SEND_FLOW_REM)

		dp.send_msg(mod)

		parser = dp.ofproto_parser
		mgmt_vlan = grt_topo["misc"]["mgmt_vlan"]

		match = parser.OFPMatch(dl_vlan=mgmt_vlan)

		actions = [parser.OFPActionOutput(ofproto.OFPP_NORMAL)]

		mod = dp.ofproto_parser.OFPFlowMod(
			datapath=dp, match=match, cookie=123,
			command=ofproto.OFPFC_MODIFY, idle_timeout=0, hard_timeout=0,
			priority=ofproto.OFP_DEFAULT_PRIORITY+100,
			flags=ofproto.OFPFF_SEND_FLOW_REM, actions=actions)

		dp.send_msg(mod)

		GRTFwdCtrl.setVM = None
		GRTFwdCtrl.nextVM = None
		GRTFwdCtrl.fromVmPaths2Cleanup = set()

		


		#TODO: Just for testing
		#num_ports = grt_topo["misc"]["num_ports"]
		#for i in range(num_ports):
		#	grt_perimeter_port = grt_topo["misc"]["perimeter_ports"][i]
		#	self.sendDrainPacket(grt_perimeter_port)


	def migrate(self, req, source_vm, dest_vm, stage, **_kwargs):
		print "MIGRATE"
		print("Shall migrate from " + source_vm + " to " + dest_vm + " on " + str(self.grt_dp.id))
		
		num_ports = grt_topo["misc"]["num_ports"]

		# Checking states prior to changing flows...

		if stage == "0":
			if GRTFwdCtrl.setVM != source_vm or GRTFwdCtrl.nextVM:
				print "setVM:" + GRTFwdCtrl.setVM + ", sourceVM:" + source_vm + ", nextVM: " + GRTFwdCtrl.nextVM
				self.fail(req, "The VM from which to migrate is not set up, or Stage 0 has already been conducted.")
				return
			GRTFwdCtrl.nextVM = dest_vm

		if stage == "1":
			if GRTFwdCtrl.setVM != source_vm or GRTFwdCtrl.nextVM != dest_vm:
				self.fail(req, "Stage 0 was not correctly called prior to Stage 1")
				return
			GRTFwdCtrl.setVM = dest_vm
			GRTFwdCtrl.nextVM = None


		#Changing flows...

		for i in range(num_ports):			

			grt_src_vm_port = grt_topo[source_vm]["ports"][i]
			grt_dest_vm_port = grt_topo[dest_vm]["ports"][i]
			grt_perimeter_port = grt_topo["misc"]["perimeter_ports"][i]

			if stage == "0":

				self.setFlow(grt_dest_vm_port, grt_perimeter_port, 0)
					#Preparing a flow from the VM away, but also keeping the last VM's one
					#In a multi-switch environment, also the source switches have to be pre
					#pared here, except the final switchover.


			elif stage == "1":

				self.setFlow(grt_perimeter_port, grt_dest_vm_port, 0)
					#Final switchover.
				GRTFwdCtrl.fromVmPaths2Cleanup.add(source_vm)
					#After a cleanup, the stale rule is removed. Cleanup should not happen too fast.

		portsToSendDrainPacket = []

		for i in range(num_ports):
			portsToSendDrainPacket.append(grt_topo[source_vm]["ports"][i])

		

		self.sendDrainPackets(portsToSendDrainPacket) #Send drain packets


		

	def sendDrainPackets(self, outPorts):
		#We first sennd a barrier request to ensure switchover has occurred, then we send the drain packet
		GRTFwdCtrl.targetsForDrainPacket = outPorts
		datapath = self.grt_dp
		ofp_parser = datapath.ofproto_parser
		req = ofp_parser.OFPBarrierRequest(datapath)
		print "Sending barrier request..."
		datapath.send_msg(req)



	def setFlow(self, inPort, outPort, timeout):

		dp=self.grt_dp
		ofproto = dp.ofproto

		print("(Re)directing ports: " + str(inPort) + " -> " + str(outPort))

		in_vlan = inPort["vlan"]
		out_vlan = outPort["vlan"]

		if (in_vlan < 0):
			match = dp.ofproto_parser.OFPMatch(in_port=inPort["openflow_portid"])
		else:
			match = dp.ofproto_parser.OFPMatch(in_port=inPort["openflow_portid"], dl_vlan=in_vlan)


		outAction = dp.ofproto_parser.OFPActionOutput(outPort["openflow_portid"])

		if out_vlan == in_vlan or (out_vlan < 0 and in_vlan < 0):
			actions = [outAction]
		elif out_vlan < 0:
			actions = [dp.ofproto_parser.OFPActionStripVlan(), outAction]
		#elif in_vlan < 0:
		#	actions = [dp.ofproto_parser.OFPActionVlanVid(out_vlan), outAction]
		else:
			actions = [dp.ofproto_parser.OFPActionVlanVid(out_vlan), outAction]

		mod = dp.ofproto_parser.OFPFlowMod(
			datapath=dp, match=match, cookie=0,
			command=ofproto.OFPFC_MODIFY, idle_timeout=timeout, hard_timeout=timeout,
			priority=ofproto.OFP_DEFAULT_PRIORITY,
			flags=ofproto.OFPFF_SEND_FLOW_REM, actions=actions)

		dp.send_msg(mod)

	def unsetFlow(self, inPort):

		dp=self.grt_dp
		ofproto = dp.ofproto

		print("Deleting flow matching " + str(inPort))

		in_vlan = inPort["vlan"]

		if (in_vlan < 0):
			match = dp.ofproto_parser.OFPMatch(in_port=inPort["openflow_portid"])
		else:
			match = dp.ofproto_parser.OFPMatch(in_port=inPort["openflow_portid"], dl_vlan=in_vlan)

		mod = dp.ofproto_parser.OFPFlowMod(
			datapath=dp, match=match, cookie=0,
			command=ofproto.OFPFC_DELETE,
			priority=ofproto.OFP_DEFAULT_PRIORITY,
			flags=ofproto.OFPFF_SEND_FLOW_REM)

		dp.send_msg(mod)

	
	def fail(self, req, strMessage):
		print("FAIL: " + strMessage)
		raise InvalidStateFailure(msg=strMessage)


class InvalidStateFailure(RyuException):
    pass

















