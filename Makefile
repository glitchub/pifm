ifeq ($(shell uname -m),armv6l)
IOBASE=0x20000000
else
IOBASE=0x3F000000
endif

CFLAGS=-Wall -Werror -pthread -lm -O3 -DIOBASE=${IOBASE}

pifm: pifm.c

.PHONY: clean
clean:;	rm -f pifm
