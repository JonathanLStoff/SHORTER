CFLAGS ?= -O1 -g -Wall -Wextra -fsanitize=undefined

sim_test: sim_test.c ../src/swd.c ../src/gd32.c ../src/swd.h ../src/gd32.h
	$(CC) $(CFLAGS) -o $@ sim_test.c ../src/swd.c ../src/gd32.c

.PHONY: test clean
test: sim_test
	./sim_test

clean:
	rm -f sim_test
