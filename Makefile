CC=clang
CXX=clang++
LD=ld.lld
.DEFAULT_GOAL := all

LIMINE_BIN = limine/limine

# Optimization level for the KERNEL. Default is -O0.
# Try:  rm -f src/*.o wyzar && make wyzar OPT=-O2
OPT ?= -O0

CFLAGS = $(OPT) -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -target x86_64-elf \
         -fno-builtin -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove \
         -fno-unwind-tables -fno-asynchronous-unwind-tables \
         -I. -Isrc -Isrc/arch -Isrc/core -Isrc/drivers -Isrc/fs -Isrc/gui

# Intel GPU bring-up + self-test + benchmark at boot. OFF by default: on the Gen9 test laptop the CPU
# copy (~0.3 ms) beats the GPU copy (~3 ms), and the self-test briefly draws test patterns on screen.
# Developer use:  make clean && make GPU=1 iso
GPU ?= 0

# Taskbar clock (src/arch/rtc.h). Windows keeps LOCAL time in the hardware clock, so 0 matches the Windows clock on
# Windows-installed test machines. If the clock is off by whole hours, set minutes to ADD to the RTC value,
# e.g. UTC-7:  make clean && make iso CLOCK_UTC_OFFSET_MIN=-420
CLOCK_UTC_OFFSET_MIN ?= 0
# 1 = show seconds in the tray (handy to see at a glance that the clock is ticking).
CLOCK_SHOW_SECONDS ?= 0
# Extra boot performance counters are opt-in; normal logs retain errors/status.
BOOT_DIAGNOSTICS ?= 0

CXXFLAGS = $(CFLAGS) -std=c++20 -fno-exceptions -fno-rtti -mno-red-zone -DGPU_BRINGUP=$(GPU) \
           -DCLOCK_UTC_OFFSET_MIN=$(CLOCK_UTC_OFFSET_MIN) -DCLOCK_SHOW_SECONDS=$(CLOCK_SHOW_SECONDS) \
           -DBOOT_DIAGNOSTICS=$(BOOT_DIAGNOSTICS)

LDFLAGS=-nostdlib -z max-page-size=0x1000

SRC=src/kernel.cpp
OBJ=$(SRC:.cpp=.o) src/gui/image.o
include src/fs/ext4_config_flags.mk
EXT4_OBJ = $(EXT4_SRC:.c=.o) src/fs/ext4_libc.o
CXXFLAGS += $(EXT4_CONFIG) $(EXT4_INC) -Isrc/fs/ext4_libc
$(OBJ) $(EXT4_OBJ): src/fs/ext4_config_flags.mk Makefile

all: wyzar init.elf counter.elf win_test.elf initrd.tar

# Backward compatibility alias
myos: wyzar

wyzar: $(OBJ) $(EXT4_OBJ) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJ) $(EXT4_OBJ)

$(EXT4_OBJ): %.o: %.c
	$(CC) -Isrc/fs/ext4_libc $(CFLAGS) -std=gnu11 $(EXT4_CONFIG) $(EXT4_INC) -MMD -MP -c $< -o $@

# The decoder uses the freestanding libc shim and the kernel heap. Optimize the
# per-pixel codec paths even when the rest of the kernel is a debug build.
src/gui/image.o: src/gui/image.cpp src/gui/image.h third_party/stb/stb_image.h
	$(CXX) -Isrc/fs/ext4_libc $(CXXFLAGS) -O2 -MMD -MP -c $< -o $@

-include $(EXT4_OBJ:.o=.d)

.storage-test/ext4-image-tool: tools/ext4_image_tool.c $(EXT4_SRC) $(wildcard third_party/lwext4/include/*.h) src/fs/ext4_config_flags.mk
	mkdir -p .storage-test
	$(CC) -O2 -std=gnu11 $(EXT4_CONFIG) $(EXT4_INC) tools/ext4_image_tool.c $(EXT4_SRC) -o $@

.storage-test/ext4-host/%.o: third_party/lwext4/src/%.c src/fs/ext4_config_flags.mk
	mkdir -p .storage-test/ext4-host
	$(CC) -O1 -g -fsanitize=address,undefined $(EXT4_CONFIG) $(EXT4_INC) -c $< -o $@

.storage-test/ext4-test: tests/ext4_test.cpp src/fs/ext4_vfs.h src/fs/vfs.h src/drivers/block.h Makefile $(patsubst third_party/lwext4/src/%.c,.storage-test/ext4-host/%.o,$(EXT4_SRC))
	$(CXX) -O1 -g -std=c++20 -no-pie -fsanitize=address,undefined $(EXT4_CONFIG) $(EXT4_INC) -Isrc/fs -Isrc/drivers $< $(filter %.o,$^) -o $@

.storage-test/autogrow-test: tests/autogrow_test.cpp src/fs/usb_autogrow.h src/fs/storage_policy.h src/fs/ext4_vfs.h src/fs/vfs.h src/drivers/block.h Makefile $(patsubst third_party/lwext4/src/%.c,.storage-test/ext4-host/%.o,$(EXT4_SRC))
	$(CXX) -O1 -g -std=c++20 -no-pie -fsanitize=address,undefined $(EXT4_CONFIG) $(EXT4_INC) -Isrc/fs -Isrc/drivers $< $(filter %.o,$^) -o $@

# Track header dependencies (wm.h, intel_gpu.h, ...) so editing a header rebuilds the kernel.
-include $(OBJ:.o=.d)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -I. -c $< -o $@

USER_CFLAGS = -O0 -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mno-red-zone -target x86_64-elf -fno-builtin \
              -fno-unwind-tables -fno-asynchronous-unwind-tables

user/init.o: user/init.c user/wiz_cmd.h src/core/wiz.h src/core/app.h src/fs/fileinfo.h
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

init.elf: user/init.o
	$(LD) -nostdlib -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $<

# A .wiz is a static ELF plus an appended package (manifest, icons, data); see
# docs/APP_PACKAGES.md. `make build/packages/NAME.wiz` packs NAME.elf using assets/apps/NAME/
# (app.manifest, optional data/) and the icon named by the manifest from the
# icon theme. Without assets/apps/NAME/app.manifest it is a plain copy, which is
# still a valid (unpackaged) .wiz. Keep NAME.elf available too.
WIZ_ASSETS = tools/wiz_pack.py $(wildcard assets/apps/*/app.manifest) $(wildcard assets/apps/*/data/*) \
             $(wildcard assets/icons/hicolor/*/apps/*.png)
PACKAGE_DIR := build/packages
PACKAGES := $(addprefix $(PACKAGE_DIR)/,counter.wiz win_test.wiz bundle_demo.wiz)
$(PACKAGE_DIR)/%.wiz: %.elf $(WIZ_ASSETS)
	mkdir -p $(@D)
	$(if $(wildcard assets/apps/$*/app.manifest),python3 tools/wiz_pack.py pack --elf $< --manifest assets/apps/$*/app.manifest --icons assets/icons/hicolor $(if $(wildcard assets/apps/$*/data),--data assets/apps/$*/data,) -o $@,cp $< $@)

packages: $(PACKAGES)
# Keep the existing sample shortcuts without creating files at the repo root.
$(notdir $(PACKAGES)): %.wiz: $(PACKAGE_DIR)/%.wiz
	@:
.PHONY: packages $(notdir $(PACKAGES))

# Host tests for the .wiz format, desktop integration, shell command and app library.
test-wiz:
	python3 tests/run_wiz_tests.py
.PHONY: test-wiz

user/counter.o: user/counter.c
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

counter.elf: user/counter.o
	$(LD) -nostdlib -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $<

user/win_test.o: user/win_test.c
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

user/bundle_demo.o: user/bundle_demo.c user/wizlib.h src/core/wiz.h
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

bundle_demo.elf: user/bundle_demo.o
	$(LD) -nostdlib -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $<

win_test.elf: user/win_test.o
	$(LD) -nostdlib -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $<

initrd.tar: init.elf counter.elf win_test.elf $(PACKAGES) assets/fonts/selawik/LICENSE.txt assets/fonts/selawik/selawk.ttf $(wildcard assets/icons/hicolor/*/apps/*.png) third_party/stb/LICENSE Makefile
	rm -rf initrd_root
	mkdir -p initrd_root/bin
	cp init.elf initrd_root/bin/
	cp counter.elf initrd_root/bin/
	cp win_test.elf initrd_root/bin/
	mkdir -p initrd_root/share/samples
	cp $(PACKAGES) initrd_root/share/samples/
	mkdir -p initrd_root/share/fonts
	cp assets/fonts/selawik/LICENSE.txt initrd_root/share/fonts/selawik.txt
	cp assets/fonts/selawik/selawk.ttf initrd_root/share/fonts/selawk.ttf
	mkdir -p initrd_root/share/icons
	cp -r assets/icons/hicolor initrd_root/share/icons/
	cp third_party/stb/LICENSE initrd_root/share/stb-image-license.txt
	tar -cf initrd.tar --format=ustar -C initrd_root .

iso: all usb-image
	rm -rf iso_root
	mkdir -p iso_root/boot

	cp wyzar iso_root/boot/
	cp init.elf iso_root/boot/
	cp initrd.tar iso_root/boot/
	cp limine.conf iso_root/               # <-- config must be named limine.conf
	cp limine/limine-bios.sys iso_root/boot/
	cp limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/

	xorriso -as mkisofs -b limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot limine-uefi-cd.bin \
	  -efi-boot-part --efi-boot-image --protective-msdos-label \
	  iso_root -o wyzar.iso

	-$(LIMINE_BIN) bios-install wyzar.iso || limine/limine.exe bios-install wyzar.iso || true


# Each ISO build refreshes USB boot/system files; user data and settings survive.
PERSIST_DISK ?= wyzar-usb.img
# Prefer a supplied firmware file; WSL installations commonly provide this path.
OVMF ?= $(firstword $(wildcard OVMF.fd /usr/share/OVMF/OVMF_CODE.fd))
ROOT_MIB ?=
DEBUG ?= 0
ifneq ($(filter $(DEBUG),0 1),$(DEBUG))
$(error DEBUG must be 0 or 1)
endif
FRESH ?= 0
ifeq ($(FRESH),1)
USB_IMAGE_FLAGS = --fresh
else
USB_IMAGE_FLAGS = --update-boot
endif
ifeq ($(DEBUG),1)
USB_IMAGE_FLAGS += --debug
endif

run: iso
	$(if $(OVMF),,$(error No OVMF firmware found; set OVMF=/path/to/OVMF_CODE.fd))
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		-bios $(OVMF) \
		-drive if=none,id=bootcd,media=cdrom,readonly=on,file=wyzar.iso \
		-device ide-cd,drive=bootcd,bootindex=1 \
		-rtc base=localtime \
		-no-reboot \
		-no-shutdown \
		-device qemu-xhci,id=xhci \
		-device usb-mouse \
		-device usb-kbd \
		-drive if=none,id=stick,format=raw,file=$(PERSIST_DISK) \
		-device usb-storage,drive=stick,bootindex=2
	


clean:
	rm -rf iso_root initrd_root $(PACKAGE_DIR) $(OBJ) $(OBJ:.o=.d) $(EXT4_OBJ) $(EXT4_OBJ:.o=.d) user/init.o user/counter.o user/win_test.o user/bundle_demo.o wyzar wyzar.iso myos myos.iso init.elf counter.elf win_test.elf bundle_demo.elf counter.wiz win_test.wiz bundle_demo.wiz initrd.tar

# Refresh boot files and bundled system apps, preserving user files/settings.
usb-image: all
	python3 tools/make_usb_image.py --output "$(PERSIST_DISK)" $(USB_IMAGE_FLAGS) $(if $(ROOT_MIB),--root-mib $(ROOT_MIB),)

# Boot directly from the USB image (uses its embedded kernel/initrd).
run-usb: usb-image
	$(if $(OVMF),,$(error No OVMF firmware found; set OVMF=/path/to/OVMF_CODE.fd))
	qemu-system-x86_64 -machine q35 -m 512M -bios $(OVMF) \
		-device qemu-xhci,id=xhci -device usb-kbd -device usb-mouse \
		-drive if=none,id=stick,format=raw,file=$(PERSIST_DISK) \
		-device usb-storage,drive=stick,bootindex=1 -rtc base=localtime

.PHONY: usb-image run-usb

# Generated atlas is checked in; this target alone requires system libfreetype6.
font-atlas:
	python3 tools/bake_ui_font.py

.PHONY: font-atlas
