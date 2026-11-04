CC=clang
CXX=clang++
LD=ld.lld

LIMINE_BIN = limine/limine

# Optimization level for the KERNEL. Default is -O0.
# Try:  rm -f src/*.o wyzar && make wyzar OPT=-O2
OPT ?= -O0

CFLAGS = $(OPT) -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -target x86_64-elf \
         -fno-builtin -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove \
         -fno-unwind-tables -fno-asynchronous-unwind-tables

# Intel GPU bring-up + self-test + benchmark at boot. OFF by default: on the Gen9 test laptop the CPU
# copy (~0.3 ms) beats the GPU copy (~3 ms), and the self-test briefly draws test patterns on screen.
# Developer use:  make clean && make GPU=1 iso
GPU ?= 0

CXXFLAGS = $(CFLAGS) -std=c++20 -fno-exceptions -fno-rtti -mno-red-zone -DGPU_BRINGUP=$(GPU)

LDFLAGS=-nostdlib -z max-page-size=0x1000

SRC=src/kernel.cpp
OBJ=$(SRC:.cpp=.o)

all: wyzar init.elf

# Backward compatibility alias
myos: wyzar

wyzar: $(OBJ) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJ)

# Track header dependencies (wm.h, intel_gpu.h, ...) so editing a header rebuilds the kernel.
-include $(OBJ:.o=.d)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -I. -c $< -o $@

USER_CFLAGS = -O0 -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mno-red-zone -target x86_64-elf -fno-builtin \
              -fno-unwind-tables -fno-asynchronous-unwind-tables

user/init.o: user/init.c
	$(CC) $(USER_CFLAGS) -I. -c $< -o $@

init.elf: user/init.o
	$(LD) -nostdlib -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $<

iso: all
	rm -rf iso_root
	mkdir -p iso_root/boot

	cp wyzar iso_root/boot/
	cp init.elf iso_root/boot/
	# Optional (ignored if missing): raw BGRA wallpaper module
	cp wallpaper.raw iso_root/boot/ || true
	cp limine.conf iso_root/               # <-- config must be named limine.conf
	cp limine/limine-bios.sys iso_root/boot/
	cp limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/

	xorriso -as mkisofs -b limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot limine-uefi-cd.bin \
	  -efi-boot-part --efi-boot-image --protective-msdos-label \
	  iso_root -o wyzar.iso

	-$(LIMINE_BIN) bios-install wyzar.iso || limine/limine.exe bios-install wyzar.iso || true


run: iso
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		-bios OVMF.fd \
		-cdrom wyzar.iso \
		-no-reboot \
		-no-shutdown \
		-device qemu-xhci,id=xhci \
		-device usb-mouse \
		-device usb-kbd
	


clean:
	rm -rf iso_root $(OBJ) $(OBJ:.o=.d) user/init.o wyzar wyzar.iso myos myos.iso init.elf
