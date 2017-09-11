# Layer 1 topology configuration for the Ryu/OpenFlow-based SliM traffic controller.
# It is currently set to the experiment setup in our evaluation, adapt it to your needs.

# On OVS, get port numbers with: ovs-vsctl -- --columns=name,ofport list Interface

# ==== VM 

vm0_port0 = {"openflow_portid":  9  , "vlan":1312}	#p2p1.1312 (dp0-vnf0)
vm0_port1 = {"openflow_portid":  9  , "vlan":1313}		#p2p1.1313 (dp1-vnf0)
vm0 = { "mgmt_ip":"172.16.20.10", "ports":[vm0_port0,vm0_port1]}


vm1_port0 = {"openflow_portid": 8, "vlan":1312}		#p2p2.1312 (dp0-vnf1)
vm1_port1 = {"openflow_portid": 8, "vlan":1313}		#p2p2.1313 (dp1-vnf1)
vm1 = { "mgmt_ip":"172.16.20.11", "ports":[vm1_port0,vm1_port1]}

# The perimeter ports are connected to the "rest" of the network.
prm0 = {"openflow_portid":  1  , "vlan":-1} # on the ctrl (dp0-ctrl).
prm1 = {"openflow_portid":  2  , "vlan":-1}	#on the hypervisor, is "2" if on the ctrl (br-dp)

#If any packet comes from mgmt_vlan, it will be simple-switched (Warning: not very secure at the moment)

misc = { "perimeter_ports":[prm0,prm1], "datapath": 40057381754576, "num_ports":2, "mgmt_vlan":1311}


grt_topo = {"vm0":vm0, "vm1":vm1, "misc":misc}

print "Topology loaded."



# List of potential VMs with their IP addresses. "Potential" means, if such a VM has registered at the ctrl,
# it has the following parameters.

