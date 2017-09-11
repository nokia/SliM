#!/bin/bash

TARGETDIR=target

DPDK_SRC=../../deps/dpdk
DPDK_TARGET=x86_64-native-linuxapp-gcc

echo "===Building GRT..."
(cd ../grt && make)

echo "=== Building Handover App..."
make

pwd

echo "=== Targeting..."
mkdir -p $TARGETDIR/lib
mkdir -p $TARGETDIR/tools

#TODO: change to cp -u ?
cp ../grt/build/libgoldenretriever.so $TARGETDIR/lib
cp handovernf configure-before-run.sh run.sh goldenretriever-example.xml $TARGETDIR/
cp $DPDK_SRC/$DPDK_TARGET/kmod/{igb_uio.ko,rte_kni.ko} $TARGETDIR/lib
cp  -a $DPDK_SRC/$DPDK_TARGET/lib/* $TARGETDIR/lib/
cp $DPDK_SRC/tools/dpdk-devbind.py $TARGETDIR/tools

