CC = gcc
CFLAGS = -Wall -Wextra -O2

all: filelist

filelist: filelist.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f filelist *.dat

.PHONY: all clean
