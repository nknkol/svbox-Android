SVBOX_DIR ?= /storage/Users/currentUser/Workspace/svbox
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra

all: binderd/svbox-binderd propd/svbox-propd

binderd/svbox-binderd: binderd/svbox_binderd.c $(SVBOX_DIR)/src/payload/include/vdev_proto.h
	$(CC) $(CFLAGS) -I$(SVBOX_DIR)/src/payload/include -o $@ $<

propd/svbox-propd: propd/svbox_propd.c
	$(CC) $(CFLAGS) -o $@ $<

stage-hnp: all
	rm -rf hap/entry/libs/arm64-v8a/libsvbox*.so
	python3 scripts/pack_hnp.py $(SVBOX_DIR)/build/svbox

clean:
	rm -f binderd/svbox-binderd propd/svbox-propd hap/entry/hnp/arm64-v8a/svbox.hnp

.PHONY: all stage-hnp clean
