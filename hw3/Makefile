CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic
LDLIBS = -pthread
PROGS = hotel_named hotel_unnamed hotel_admin hotel_client

all: $(PROGS)

hotel_named: hotel_fork.c common.c sync_named.c hotel.h
	$(CC) $(CFLAGS) hotel_fork.c common.c sync_named.c -o $@ $(LDLIBS)

hotel_unnamed: hotel_fork.c common.c sync_unnamed.c hotel.h
	$(CC) $(CFLAGS) hotel_fork.c common.c sync_unnamed.c -o $@ $(LDLIBS)

hotel_admin: hotel_admin.c common.c sync_named.c hotel.h
	$(CC) $(CFLAGS) hotel_admin.c common.c sync_named.c -o $@ $(LDLIBS)

hotel_client: hotel_client.c common.c sync_named.c hotel.h
	$(CC) $(CFLAGS) hotel_client.c common.c sync_named.c -o $@ $(LDLIBS)

test: all
	./tests/run_tests.sh

clean:
	rm -f $(PROGS)
	rm -rf tests/out

.PHONY: all test clean
