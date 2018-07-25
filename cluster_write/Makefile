CC=g++ -std=c++11 -g  
OFED_PATH=/usr
DEFAULT_CFLAGS=-I${OFED_PATH}/include 
DEFAULT_LDFLAGS=-L${OFED_PATH}/lib64 -L${OFED_PATH}/lib 

LDFLAGS+=$(DEFAULT_LDFLAGS) -libverbs -lpthread 
OBJECTS =hello.o sock.o 
TARGETS=hello 

all:${TARGETS}

hello:hello.o sock.o
	$(CC) $^ -o $@ $(LDFLAGS)

hello.o:hello.cpp sock.h
	$(CC) -c $(CFLAGS) $<

sock.o: sock.cpp sock.h
	$(CC) -c $(CFLAGS) $<

clean:
	rm -f $(OBJECTS) $(TARGETS)
