# Original regression for generator patch 0021: what must stay reported. No game bytes.
#_ XEX_ENTRY _start

.text
.globl _start
_start:
    li r3,0
    blr

# A jump table whose index has no bound (no guarding cmplwi, the table is not
# inline): XenonAnalyse reports it as unresolved; the bctr stays an indirect call.
.globl fn_unbounded
fn_unbounded:
    lis r12,.Lu_table@ha
    addi r12,r12,.Lu_table@l
    rlwinm r0,r3,2,0,29
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Lu0:
    li r3,1
    blr
.Lu1:
    li r3,2
    blr

# A declared function whose straight-line flow reaches a word that does not
# decode: "Unable to decode" stays a diagnostic and the generated code traps there.
.globl fn_undecodable
fn_undecodable:
    li r3,3
    .long 0x00485645
    blr

.section .rodata
.balign 4
.Lu_table:
    .long .Lu0, .Lu1
