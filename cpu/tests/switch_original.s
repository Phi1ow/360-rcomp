# Original regression, no game code. Remove only dispatcher pdata entries
# in the synthetic image to exercise heuristic switch discovery.
#_ XEX_ENTRY _start
.text
.globl _start
_start:
    cmpwi cr0,r4,0
    beq cr0,fn_switch
    b fn_duplicate
.globl fn_switch
fn_switch:
    cmplwi cr6,r3,3
    bgt cr6,.Labs_default
    lis r11,table_abs@ha
    addi r11,r11,table_abs@l
    rlwinm r0,r3,2,0,29
    lwzx r12,r11,r0
    mtctr r12
    bctr
    .space 16
.Labs_zero:
    li r3,10
    blr
.Labs_one:
    li r3,20
    blr
.Labs_two:
    li r3,30
    blr
.Labs_three:
    li r3,40
    blr
.Labs_default:
    li r3,99
    blr
.globl fn_duplicate
fn_duplicate:
    cmplwi cr6,r3,3
    bgt cr6,.Ldup_default
    cmpwi cr0,r3,1
    beq cr0,.Ldup_case
    lis r11,table_dup@ha
    addi r11,r11,table_dup@l
    rlwinm r0,r3,2,0,29
    lwzx r12,r11,r0
    mtctr r12
    bctr
    .space 32
.Ldup_case:
    li r3,77
    blr
.Ldup_default:
    li r3,88
    blr
.globl fn_boundary
fn_boundary:
    li r3,123
    blr
.section .rodata
.balign 4
table_abs:
    .long .Labs_zero,.Labs_one,.Labs_two,.Labs_three
table_dup:
    .long .Ldup_case,.Ldup_default,.Ldup_case,.Ldup_default
.data
.long 0
