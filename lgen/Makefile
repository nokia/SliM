CC=gcc

# Only for grt-lgen!

LDLIBS += -ldl
LDLIBS += -lpthread
LDLIBS += -lm

app: grt-lgen grt-lgen-handover

grt-lgen: lgen_main.o
	$(CC) $(LDDIRS) -o grt-lgen lgen_main.o $(LDLIBS)
	
grt-lgen-handover: lgen_handover.o
	$(CC) $(LDDIRS) -o grt-lgen-handover lgen_handover.o $(LDLIBS)

lgen_main.o: lgen_main.c
	$(CC) -mssse3 -c lgen_main.c
	
lgen_handover.o: lgen_handover.c
	$(CC) -mssse3 -c lgen_handover.c

