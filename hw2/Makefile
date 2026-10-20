CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic
LDLIBS = -pthread

all: hotel_sem hotel_cond

hotel_sem: hotel.c sync_sem.c hotel.h
	$(CC) $(CFLAGS) hotel.c sync_sem.c -o $@ $(LDLIBS)

hotel_cond: hotel.c sync_cond.c hotel.h
	$(CC) $(CFLAGS) hotel.c sync_cond.c -o $@ $(LDLIBS)

test: all
	./tests/run_tests.sh

clean:
	rm -f hotel_sem hotel_cond
	rm -rf tests/out

.PHONY: all test clean
