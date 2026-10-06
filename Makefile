CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic

hotel: hotel.c
	$(CC) $(CFLAGS) $< -o $@

test: hotel
	./tests/run_tests.sh

clean:
	rm -f hotel
	rm -rf tests/out

.PHONY: test clean
