SVBOX_DIR ?= /storage/Users/currentUser/Workspace/svbox
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra

all: binderd/svbox-binderd propd/svbox-propd

binderd/svbox-binderd: binderd/svbox_binderd.c $(SVBOX_DIR)/src/payload/include/vdev_proto.h
	$(CC) $(CFLAGS) -I$(SVBOX_DIR)/src/payload/include -o $@ $<

propd/svbox-propd: propd/svbox_propd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f binderd/svbox-binderd propd/svbox-propd

.PHONY: all clean
