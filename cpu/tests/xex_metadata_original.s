# Original metadata fixture, no game content or imported code.
#_ XEX_ENTRY start
#_ XEX_HEADER_VALUE 0xCAFE0000 0x12345678
#_ XEX_HEADER_VALUE 0xCAFE0100 0
#_ XEX_HEADER_VALUE 0xCAFE0201 0xA1B2C3D4
#_ XEX_HEADER_DATA 0xCAFE0302 header_fixed 8
#_ XEX_HEADER_DATA 0xCAFE04FF header_variable 12
#_ XEX_HEADER_VALUE 0x00030000 0x00200008

.text
.align 2
.globl start
start:
    li r3,42
    blr

.section .rodata
.align 2
header_fixed:
    .long 0x13579BDF,0x2468ACE0
header_variable:
    .long 12,0x10203040,0x50607080

.data
.align 2
metadata_data:
    .long 0x11223344
