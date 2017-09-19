**SliM** is a proposed method for *seamless state migration* of high-performance virtual network functions (VNFs), 
a.k.a. middleboxes. This implementation is an extension for DPDK and adds seamless state migration support to packet-processing
applications using it.

Note that the current SliM implementation version is **experimental software** and not yet intended for productive usage.
SliM is distributed software, so we have several components which must be built and installed separately.

### About the directories:

- *grt:* Core of the SliM depending on DPDK. NF implementations (like nat_nf or handover_nf) depend on this module.
- *nat_nf:* NAT network function using SliM, installed in a VM or on a bare-metal device.
- *handover_nf:* Simple packet gatways example network function using SliM, installed in a VM or on a bare-metal device.
- *ctrl:* Python/Ryu-based controller for managing SliM-based network functions in an OpenFlow network.
- *lgen:* Load generator tools that can be used to evaluate the network functions and the SliM migration process.
- *posteval:* Post-evaluation tools (aggregation, plotting etc.)

See INSTALL.md for more information on installation.

#Installation

For more detailed instructions, follow instructions in the subfolders and files.

### ctrl (OpenFlow-based datapath controller)

Required: ryu

Fit ctrl/topo.py to:
 - the port numbers on your dataplane device
 - the vlan IDs, if there, or '',
 - the datapath ID of the switch.

Start ryu with ctrl/fwd.py as the Ryu app

Start controller_rest.py in a separate process, which will spawn a command line to manually migrate VMs (for testing), and also expose a web service
which can be used by higher-layer controllers.

### grt (Core SliM files)

Core files of SliM. Must be deployed on the SliM-enabled NF hosts (e.g. VMs).

### handover_nf, nat_nf (Example network function implementations)

Example implementations of SliM-enabled network functions. Require DPDK and the **grt** directory.
See grt/Instructions.txt on how to build and start up.

### lgen (Evaluation tools)

Load generator and evaluator scripts which can be used to test the NF performance. 
Currently customized to produce and measure nat_nf and handover_nf performance. 

The Makefile is only for building the performance-critical load generator. 
The scripts will explain their usage on running them. 
orch.py is an orchestrator script which can be used to automate evaluation runs.

### posteval (Post-evaluation tools)

Contains finalize.py and some gnuplot scripts.
finalize.py must be adapted to the selected evaluation configuration in orch.py
After running it, resulting CSV records can be plotted with Gnuplot and the scripts in posteval/plotting.


## Further references

Early version of a PPPoE network function compatible with SliM: https://github.com/tudps/slim-pppoe (GPLv3-licensed)
