CC     = gcc
CFLAGS = -O2 -Wall -Wextra 
LDLIBS = -libverbs

OBJS = kvwire_main.o kvwire_rdma.o kvwire_pcie.o kvwire_pipe.o

kvwire: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): kvwire.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f kvwire $(OBJS)

.PHONY: clean
