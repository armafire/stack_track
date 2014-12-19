URCUDIR ?= /usr/local

CC = gcc
LD = gcc

CFLAGS += -I$(URCUDIR)/include
CFLAGS += -D_REENTRANT
#CFLAGS += -DNDEBUG
CFLAGS += -g -mrtm -O3

CFLAGS += -Winline --param inline-unit-growth=1000 

LDFLAGS += -L$(URCUDIR)/lib
LDFLAGS += -lpthread

BINS = bench-skiplist 

.PHONY:	all clean

all: $(BINS)

common.o: common.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

atomics.o: atomics.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

htm.o: htm.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

stack-track.o: stack-track.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

skip-list.o: skip-list.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

bench.o: bench.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

bench-skiplist: common.o atomics.o htm.o stack-track.o skip-list.o bench.o
	$(LD) -o $@ $^ $(LDFLAGS) $(LDURCU)

clean:
	rm -f $(BINS) *.o
