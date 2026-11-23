SVBOX_DIR ?= /storage/Users/currentUser/Workspace/svbox
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra

all: binderd/svbox-binderd propd/svbox-propd netd/svbox-netd audio_hal/audio.primary.svbox.so audio_hal/svbox-audio-service camera_stub/svbox-cameraserver

binderd/svbox-binderd: binderd/svbox_binderd.c $(SVBOX_DIR)/src/payload/include/vdev_proto.h
	$(CC) $(CFLAGS) -I$(SVBOX_DIR)/src/payload/include -o $@ $<

propd/svbox-propd: propd/svbox_propd.c
	$(CC) $(CFLAGS) -o $@ $<

netd/svbox-netd: netd/svbox_netd.c $(SVBOX_DIR)/src/payload/include/vdev_proto.h $(SVBOX_DIR)/src/payload/include/net_proto.h
	$(CC) $(CFLAGS) -pthread -I$(SVBOX_DIR)/src/payload/include -o $@ $<

audio_hal/audio.primary.svbox.so: audio_hal/audio_primary_svbox.c
	$(CC) -O2 -Wall -Wextra -fPIC -shared -nostdlib -fno-stack-protector -fno-builtin -Wl,-soname,audio.primary.default.so -o $@ $<

audio_hal/svbox-audio-service: audio_hal/svbox_audio_service.cpp
	clang++ -O2 -Wall -Wextra -fPIE -pie -nostdlib -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-e,_start -Wl,--no-as-needed -o $@ $< rootfs/system/apex/com.android.runtime/lib64/bionic/libc.so rootfs/system/apex/com.android.runtime/lib64/bionic/libdl.so rootfs/system/lib64/libhidlbase.so

camera_stub/svbox-cameraserver: camera_stub/svbox_cameraserver.c
	$(CC) -O2 -Wall -Wextra -fPIE -pie -nostdlib -fno-stack-protector -fno-builtin -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-e,_start -Wl,--no-as-needed -o $@ $< rootfs/system/apex/com.android.runtime/lib64/bionic/libc.so rootfs/system/apex/com.android.runtime/lib64/bionic/libdl.so

stage-hnp: all
	rm -rf hap/entry/libs/arm64-v8a/libsvbox*.so
	@if [ -f gpu/build_host/libgfxstream_host.so ]; then \
		mkdir -p hap/entry/libs/arm64-v8a; \
		cp gpu/build_host/libgfxstream_host.so hap/entry/libs/arm64-v8a/libgfxstream_host.so; \
		/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0/sdk/default/openharmony/native/llvm/bin/llvm-strip --strip-unneeded hap/entry/libs/arm64-v8a/libgfxstream_host.so || true; \
		ls -lh hap/entry/libs/arm64-v8a/libgfxstream_host.so; \
	fi
	python3 scripts/pack_hnp.py $(SVBOX_DIR)/build/svbox

clean:
	rm -f binderd/svbox-binderd propd/svbox-propd netd/svbox-netd audio_hal/audio.primary.svbox.so audio_hal/svbox-audio-service camera_stub/svbox-cameraserver hap/hnp/arm64-v8a/svbox.hnp

.PHONY: all stage-hnp clean
