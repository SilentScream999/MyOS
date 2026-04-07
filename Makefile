CC=clang
CXX=clang++
LD=ld.lld

LIMINE_BIN = limine/limine

CFLAGS = -O0 -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mcmodel=kernel -target x86_64-elf \
         -fno-builtin -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove \
         -fno-unwind-tables -fno-asynchronous-unwind-tables

CXXFLAGS = $(CFLAGS) -std=c++20 -fno-exceptions -fno-rtti -mno-red-zone

LDFLAGS=-nostdlib -z max-page-size=0x1000

SRC=src/kernel.cpp
OBJ=$(SRC:.cpp=.o)

all: myos init.elf

myos: $(OBJ) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I. -c $< -o $@

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

	cp myos iso_root/boot/
	cp init.elf iso_root/boot/
	cp limine.conf iso_root/               # <-- config must be named limine.conf
	cp limine/limine-bios.sys iso_root/boot/
	cp limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/

	xorriso -as mkisofs -b limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot limine-uefi-cd.bin \
	  -efi-boot-part --efi-boot-image --protective-msdos-label \
	  iso_root -o myos.iso

	-$(LIMINE_BIN) bios-install myos.iso || limine/limine.exe bios-install myos.iso || true


run: iso
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		-bios OVMF.fd \
		-cdrom myos.iso \
		-no-reboot \
		-no-shutdown \
		-device qemu-xhci,id=xhci \
		-device usb-mouse \
		-device usb-kbd
	


clean:
	rm -rf iso_root $(OBJ) user/init.o myos myos.iso init.elf
