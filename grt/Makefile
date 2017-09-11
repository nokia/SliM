CC=gcc

# Should contain pre-built DPDK at least.
RTE_SDK=/home/lnobach/slim/deps/dpdk

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

INC=-I$(RTE_SDK)/$(RTE_TARGET)/include
INC+=-I/usr/include/libxml2	#This is for Ubuntu building. Might be that your libxml2 is somewhere else.

INCLINK=-L$(RTE_SDK)/$(RTE_TARGET)/lib

BUILDDIR=build

app: $(BUILDDIR)/grt_main.o $(BUILDDIR)/grt_toolz.o $(BUILDDIR)/grt_redirect_table.o
	$(CC) $(INCLINK) -ldpdk -shared -o $(BUILDDIR)/libgoldenretriever.so $(BUILDDIR)/grt_main.o $(BUILDDIR)/grt_toolz.o $(BUILDDIR)/grt_redirect_table.o

$(BUILDDIR)/grt_main.o: grt_main.c build
	cd $(BUILDDIR)
	$(CC) -mssse3 -fPIC $(INC) -c grt_main.c -o $(BUILDDIR)/grt_main.o

$(BUILDDIR)/grt_toolz.o: grt_toolz.c build
	cd $(BUILDDIR)
	$(CC) -mssse3 -fPIC $(INC) -c grt_toolz.c -o $(BUILDDIR)/grt_toolz.o

$(BUILDDIR)/grt_redirect_table.o: grt_redirect_table.c build
	cd $(BUILDDIR)
	$(CC) -mssse3 -fPIC $(INC) -c grt_redirect_table.c -o $(BUILDDIR)/grt_redirect_table.o

build:
	mkdir -p build

