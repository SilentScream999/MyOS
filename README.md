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

* [x] 64-bit flat GDT setup
* [x] Kernel + user segment preparation (ring 0 / ring 3)

## IDT (Interrupt Descriptor Table)

* [x] IDT setup

* [x] Exception handlers:

  * [x] Divide by zero
  * [x] General protection fault
  * [x] Page fault
  * [x] Invalid opcode
  * [x] Double fault

* [x] Kernel panic system with framebuffer output

## IRQ + Interrupt Handling

* [x] IRQ remapping (PIC or APIC)
* [x] Interrupt dispatcher system
* [x] Pluggable IRQ handler system

## Timer System

* [x] PIT or APIC timer setup
* [x] Global tick counter
* [x] Basic sleep/delay functions
---

# 🧠 PHASE 2 — MEMORY MANAGEMENT

## Physical Memory Manager (PMM)

* [x] Parse Limine memory map
* [x] Frame allocator (bitmap or stack)
* [x] alloc_frame / free_frame

## Virtual Memory Manager (VMM)

* [x] Page table manager (4-level paging)
* [x] map_page / unmap_page
* [x] Kernel heap region setup
* [x] User vs kernel memory separation prep

## Kernel Heap

* [x] kmalloc / kfree
* [x] bump allocator → slab allocator later
* [x] Heap built on VMM

---

# 🟣 PHASE 3 — PROCESS SYSTEM

## Task Model

* [x] PCB (PID, registers, stack, state, page table)

## Context Switching

* [x] Save/restore CPU state
* [x] Stack switching
* [x] CR3 switching

## Scheduler

* [x] Round-robin scheduler (single core first)
* [x] Task queue system
* [x] Timer-driven scheduling (later)

---

# 🟠 PHASE 4 — SYSCALLS

## Syscall Interface

* [x] syscall/sysret entry
* [x] syscall dispatcher

## Minimal Syscalls

* [x] sys_write
* [x] sys_exit
* [x] sys_getpid

## User Mode Prep

* [x] Ring 3 transition
* [x] User stack setup

---

# 🟡 PHASE 5 — VFS + FILESYSTEM

## VFS

* [ ] open / read / write / close abstraction
* [ ] path resolution

## Ramdisk FS

* [ ] in-memory filesystem backend

## Future FS

* [ ] ext2 or custom filesystem

# 🔴 PHASE 6 — ELF LOADER + USERSPACE [COMPLETED]
  
* [x] ELF64 parsing
* [x] segment mapping
* [x] entry execution
* [x] execve-style loading
* [x] user address space setup
* [x] Ring 3 transition (IRETQ/SYSRETQ stable)
* [x] first init program running and using syscalls

---

# 🟤 PHASE 7 — TERMINAL & TTY [COMPLETED]

## Kernel TTY

* [x] /dev/tty abstraction (tty.h buffer & line discipline)
* [x] input buffer system handling USB scancodes

## Userspace Terminal

* [x] framebuffer terminal emulator (terminal.h)
* [x] reads from TTY (via updated sys_read)
* [x] ANSI escape sequence support (\e[2J, \e[H)
* [x] Margin support to prevent screen clipping

## Shell

* [x] command parser (in user/init.c)
* [x] program execution (future/execve)
* [x] basic built-ins (clear, exit/disabled, shutdown)

---

# 🟢 PHASE 8 — DEVICE DRIVERS & KERNEL POLISH [COMPLETED]

## Persistent Terminal & Kernel Logging

* [x] Terminal loads independently of keyboard connection
* [x] All hardware/kernel `print()` logs redirected to `bootlog.txt`
* [x] `open bootlog` command to view logs in the shell

## Input

* [x] USB keyboard (XHCI working, hub support, arrow keys, key repeat)
* [x] Raw mode TTY (`SYS_TTYRAW`) for interactive line input
* [ ] input event system abstraction

## Rendering Engine (Speed King)

* [x] 64KB circular kernel log buffer
* [x] Double-buffering with backbuffer + dirty-line tracking
* [x] SSE non-temporal store acceleration (MOVNTDQ)
* [x] Sub-millisecond partial screen flips

## Shell UX

* [x] Underscore cursor (VRAM-transient, no ghost artifacts)
* [x] Command history (Up/Down arrows)
* [x] Shift+Arrow hardware scrollback
* [x] Green command highlighting (real-time, pre-Enter)

---

# 🖥️ PHASE 9 — VISUAL DESKTOP

The next big milestone: a real graphical desktop environment running on top of our kernel.

## Window Manager

* [ ] Framebuffer surface / canvas abstraction
* [ ] Draggable window regions with title bars
* [ ] Z-order layering (front/back window management)
* [ ] Mouse input (USB HID mouse driver)

## UI Rendering

* [ ] Filled rectangle + rounded corner primitives
* [ ] Font rendering at multiple sizes
* [ ] Icon/sprite blitting system
* [ ] Desktop wallpaper / background

## Desktop Shell

* [ ] Taskbar with clock and running app list
* [ ] Right-click context menu
* [ ] App launcher / start menu
* [ ] File browser (backed by VFS)

## First Apps

* [ ] Terminal emulator window (port existing TTY shell)
* [ ] Simple text editor
* [ ] System info viewer

---

# 🔮 FUTURE REFERENCE — Lower-Level Infrastructure

* [ ] Device Drivers (PCI/SATA/NVMe)
* [ ] More syscalls (open, close, fork, waitpid)
* [ ] Proper VFS with real storage backing
* [ ] Dynamic Memory Management improvements

## Graphics / IO (Remaining)

* [ ] framebuffer abstraction layer
* [ ] PCI enumeration
* [ ] storage drivers (SATA/NVMe later)
