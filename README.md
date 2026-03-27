RUN IT AND FIND OUT!


You will need WSL on Windows. Install it with Ubuntu-22.04 for the best outcome.

This project depends on Qemu, Xorriso, Make, and Clang++

It is recommended that you work on this project using CodeWizard2 (Available at https://github.com/AdamJosephMather/CodeWizard2)

**Important**: Limine is a little silly (We didn't link it properly) so we might link our own repo for usage with this OS or we'll fix it to link the official distro, depending on how we feel!


Note:
	
	To get limine working, make sure to clone the BINARY branch into the limine folder! 
	
	Ex: git clone https://github.com/limine-bootloader/limine.git --branch=v11.1.0-binary --depth=1


# 🧠 OS Development Roadmap (x86_64 Limine Kernel)

## 🎯 Project Goal

Build a modular 64-bit operating system kernel with:

* hardware abstraction layer
* memory management
* multitasking (processes + scheduler)
* syscall interface
* ELF userspace execution
* VFS + filesystem
* eventual userspace terminal + shell

Framebuffer output is already working and used for early debugging.

---

# 🟢 CURRENT STATUS (COMPLETED / IN PROGRESS)

### Boot & Output

* [x] Limine bootloader integration
* [x] 64-bit long mode kernel
* [x] Framebuffer initialization
* [x] On-screen debug output system

### Input

* [x] USB keyboard input via XHCI

---

# 🔵 PHASE 1 — CPU + INTERRUPTS FOUNDATION

## CPU Identification + Setup

* [ ] CPUID implementation
* [ ] CPU feature detection (SSE, APIC, etc.)
* [ ] Per-core CPU info struct
* [ ] Validate long mode and paging features

## GDT (Global Descriptor Table)

* [ ] 64-bit flat GDT setup
* [ ] Kernel + user segment preparation (ring 0 / ring 3)

## IDT (Interrupt Descriptor Table)

* [ ] IDT setup

* [ ] Exception handlers:

  * [ ] Divide by zero
  * [ ] General protection fault
  * [ ] Page fault
  * [ ] Invalid opcode
  * [ ] Double fault

* [ ] Kernel panic system with framebuffer output

## IRQ + Interrupt Handling

* [ ] IRQ remapping (PIC or APIC)
* [ ] Interrupt dispatcher system
* [ ] Pluggable IRQ handler system

## Timer System

* [ ] PIT or APIC timer setup
* [ ] Global tick counter
* [ ] Basic sleep/delay functions
---

# 🧠 PHASE 2 — MEMORY MANAGEMENT

## Physical Memory Manager (PMM)

* [ ] Parse Limine memory map
* [ ] Frame allocator (bitmap or stack)
* [ ] alloc_frame / free_frame

## Virtual Memory Manager (VMM)

* [ ] Page table manager (4-level paging)
* [ ] map_page / unmap_page
* [ ] Kernel heap region setup
* [ ] User vs kernel memory separation prep

## Kernel Heap

* [ ] kmalloc / kfree
* [ ] bump allocator → slab allocator later
* [ ] Heap built on VMM

---

# 🟣 PHASE 3 — PROCESS SYSTEM

## Task Model

* [ ] PCB (PID, registers, stack, state, page table)

## Context Switching

* [ ] Save/restore CPU state
* [ ] Stack switching
* [ ] CR3 switching

## Scheduler

* [ ] Round-robin scheduler (single core first)
* [ ] Task queue system
* [ ] Timer-driven scheduling (later)

---

# 🟠 PHASE 4 — SYSCALLS

## Syscall Interface

* [ ] syscall/sysret entry
* [ ] syscall dispatcher

## Minimal Syscalls

* [ ] sys_write
* [ ] sys_exit
* [ ] sys_getpid

## User Mode Prep

* [ ] Ring 3 transition
* [ ] User stack setup

---

# 🟡 PHASE 5 — VFS + FILESYSTEM

## VFS

* [ ] open / read / write / close abstraction
* [ ] path resolution

## Ramdisk FS

* [ ] in-memory filesystem backend

## Future FS

* [ ] ext2 or custom filesystem

---

# 🔴 PHASE 6 — ELF LOADER + USERSPACE

## ELF Loader

* [ ] ELF64 parsing
* [ ] segment mapping
* [ ] entry execution

## Process Execution

* [ ] execve-style loading
* [ ] user address space setup

## First Userspace Programs

* [ ] init
* [ ] echo
* [ ] minimal shell

---

# 🟤 PHASE 7 — TERMINAL (USERSpace)

## Kernel TTY

* [ ] /dev/tty abstraction
* [ ] input buffer system

## Userspace Terminal

* [ ] framebuffer terminal emulator
* [ ] reads from TTY

## Shell

* [ ] command parser
* [ ] program execution
* [ ] basic built-ins

---

# 🟢 PHASE 8 — DEVICE DRIVERS

## Input

* [x] USB keyboard (XHCI working)
* [ ] input event system

## Graphics / IO

* [ ] framebuffer abstraction layer
* [ ] PCI enumeration
* [ ] storage drivers (SATA/NVMe later)

---
