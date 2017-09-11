#!/bin/bash

DPDK_DEVBIND=tools/dpdk-devbind.py

#Required kernel modules
#modprobe uio
#insmod lib/igb_uio.ko
#insmod lib/rte_kni.ko

modprobe uio
insmod lib/igb_uio.ko

#The following must be done for every device we want to use.

$DPDK_DEVBIND --bind igb_uio 00:10.0
$DPDK_DEVBIND --bind igb_uio 00:11.0

export LD_LIBRARY_PATH=./lib
./demonf -c7 -n4 -w 00:10.0 -w 00:11.0 -d  lib/librte_pmd_nfp.so $1 $2
