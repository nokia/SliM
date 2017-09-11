#!/bin/bash
mkdir -p /mnt/huge
umount -f /mnt/huge
mount -t hugetlbfs nodev /mnt/huge
echo 64 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
cat /proc/meminfo

