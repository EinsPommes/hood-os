OBJDIR := ../../../../build/rootfs/bin
BINDIR := ../../../../build/rootfs

CC := gcc
CFLAGS := -Wall -Wextra -O2

SRCS := init.c
OBJS := $(SRCS:.c=.o)

all: $(BINDIR)/init

$(BINDIR)/init: $(OBJS)
	mkdir -p $(BINDIR)
	$(CC) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BINDIR)/init
