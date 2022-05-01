CC=gcc
CFLAGS := -Wall -O2

da65ify: da65ify.o
	$(CC) $< -o $@ $(LDFLAGS)

%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@
