CC = gcc

PROJ=slim_ofdpa_native

INC=-I/usr/include/ofdpa-2.0-ga
INC+=-I/usr/include/python2.7 -lpython2.7 
INCLINK=-L/lib/x86_64-linux-gnu



app: $(PROJ).o
	$(CC) $(INCLINK) -shared -o $(PROJ).so /lib/x86_64-linux-gnu/libofdpa_rpc_client.so.1 $(PROJ).o


$(PROJ).o: src/$(PROJ).c
	$(CC) -fPIC $(INC) -c src/$(PROJ).c

