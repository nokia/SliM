CC=gcc

# Should contain pre-built DPDK at least.
RTE_SDK=/home/lnobach/slim/deps/dpdk

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

LDDIRS += -L../grt/build	#Adapt this if you have libgoldenretriever.so compiled somewhere else.
LDDIRS += -L$(RTE_SDK)/$(RTE_TARGET)/lib	#Here, libdpdk.so should reside.

LDLIBS += -lgoldenretriever
LDLIBS += -ldpdk
LDLIBS += -ldl
LDLIBS += -lpthread
LDLIBS += -lxml2 
LDLIBS += -lm

app: main.o
	$(CC) $(LDDIRS) -o handovernf main.o $(LDLIBS)

main.o: main.c
	$(CC) -mssse3 -I../grt -I$(RTE_SDK)/$(RTE_TARGET)/include -c main.c

