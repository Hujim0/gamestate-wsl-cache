CC ?= cc
CFLAGS ?= -Os -std=c11 -Wall -Wextra -Wpedantic

all: gamestatus

gamestatus: gamestatus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f gamestatus

.PHONY: all clean
