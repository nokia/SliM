Implementation of the SliM state migration datapath on the Broadcom OF-DPA silicon API.
Successfully tested on an AS5712-54x with OpenNetworkLinux.

1. Install the OF-DPA driver for your switch vendor: https://github.com/Broadcom-Switch/of-dpa

2. Put some dependencies into the "deps" directory. This is the tree how it should look like. 
All files can be drawn from an OF-DPA binary package. The bintools are not required but may help with debugging.

deps
├── bintools
│   ├── client_cfg_purge
│   ├── client_debugcomp
│   ├── client_debuglvl
│   ├── client_drivshell
│   ├── client_event
│   ├── client_flowtable_dump
│   ├── client_grouptable_dump
│   ├── client_oam_dump
│   ├── client_port_table_dump
│   └── client_tunnel_dump
├── OFDPA_python.py
└── _OFDPA_python.so

3. Adapt "install-on-switch.sh" to your needs.

4. Hit "make" on the switch to build the binary parts.

5. Start with "./main.py"


You can debug OF-DPA very well if you create a file /usr/sbin/ofdpa_debug (must be executable with chmod 755, of course):

    #!/bin/sh

    /usr/sbin/ofdpa > /tmp/ofdpa.log 2>&1

and  change "NAME=ofdpa" to "NAME=ofdpa_debug" in /etc/init.d/ofdpa

Then either restart the switch or restart the daemon only (which sometimes does not work), finally set the debug level with the binaries "client_debugcomp" and "client_debuglvl" to your needs.

Then you will find a pretty explanation in /tmp/ofdpa.log why your flow could not be added and was rejected.


Usage:

Start controller-rest.py (in the ctrl dir), but now the fwd.py (Ryu, OpenFlow) component is replaced with this one here.

