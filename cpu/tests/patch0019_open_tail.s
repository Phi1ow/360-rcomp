# Original negative regression for generator patch 0019. No game bytes.
# The tail [.Lopen_tail, end) branches back before itself, so it is not
# closed: the branch into it must keep its "Direct call" diagnostic.
#_ XEX_ENTRY _start
.text
.globl _start
_start:
    li r3,0
    blr

.globl fn_loop_owner
fn_loop_owner:
    li r3,0
.Lloop:
    addi r3,r3,1
.Lopen_tail:
    cmpwi cr6,r3,10
    blt cr6,.Lloop
    blr

.globl fn_open_jump
fn_open_jump:
    li r3,5
    b .Lopen_tail
