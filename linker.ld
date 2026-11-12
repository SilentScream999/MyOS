OUTPUT_FORMAT(elf64-x86-64)

/* kmain is our entry */
ENTRY(kmain)

PHDRS {
	limine_requests PT_LOAD;
	text PT_LOAD;
	rodata PT_LOAD;
	data PT_LOAD;
}

SECTIONS {
	/* Limine mandates the higher-half (topmost 2GiB). */
	. = 0xffffffff80000000;
	_kernel_start = .;

	/* Limine requests live in a dedicated segment. */
	.limine_requests : {
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
	} :limine_requests

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.text : { *(.text .text.*) } :text

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.rodata : { *(.rodata .rodata.*) } :rodata

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.data : { *(.data .data.*) } :data

	/* Keep .bss last in :data */
	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	} :data
	_kernel_end = .;

	/DISCARD/ : {
		*(.eh_frame*)
		*(.note .note.*)
	}
}
