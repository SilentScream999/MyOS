# lwext4 integration

Upstream: https://github.com/gkostka/lwext4

Pinned revision: `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`.
Downloaded 2026-11-10. Upstream source is unmodified.

This is a source snapshot, not a Git submodule. The kernel builds the library's
filesystem/journal sources, excluding its formatter and partition builder.
Configuration is in `src/fs/ext4_config_flags.mk`; private libc and allocator
bindings live in `src/fs/ext4_libc*` and `src/kernel.cpp`.

Licenses vary by file. Keep the upstream LICENSE and all source notices.
In particular extents and xattrs include GPL-2.0-or-later code; this dependency
must not be represented as wholly BSD licensed. Binary redistribution needs
the corresponding license/source obligations addressed. The system image
includes `/share/lwext4-license.txt`.

The adapter flushes the underlying device after each completed library write
because the upstream block interface has no separate ordering/barrier callback.
Do not remove those flushes without implementing and testing equivalent ordering
for journal descriptor/data/commit/checkpoint writes.
