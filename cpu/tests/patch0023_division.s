# Original regression for generator patch 0023 (non-trapping integer division).
# No game bytes. Every global .text label becomes a .pdata function
# (cpu/tools/mkxex.py). Inputs: r3 = dividend, r4 = divisor; result in r3.
# The forms a generator without patch 0023 also translates (divw divwu divd
# divdu, divw. divwu. divdu.) are here; the OE forms are in patch0023_overflow.s.
#_ XEX_ENTRY _start

.text
.globl _start
_start:
    li r3,0
    blr

.globl fn_divw
fn_divw:
    divw r3,r3,r4
    blr

.globl fn_divwu
fn_divwu:
    divwu r3,r3,r4
    blr

.globl fn_divd
fn_divd:
    divd r3,r3,r4
    blr

.globl fn_divdu
fn_divdu:
    divdu r3,r3,r4
    blr

# Record forms: CR0 from the result (as the generator emitted it before patch 0023).
.globl fn_divw_rc
fn_divw_rc:
    divw. r3,r3,r4
    blr

.globl fn_divwu_rc
fn_divwu_rc:
    divwu. r3,r3,r4
    blr

.globl fn_divdu_rc
fn_divdu_rc:
    divdu. r3,r3,r4
    blr

# rD == rB: the quotient of the old r4.
.globl fn_divw_alias
fn_divw_alias:
    divw r4,r3,r4
    mr r3,r4
    blr

# The shape that stopped Halo 3 (sub_824E2AF8): the quotient is computed before
# the divisor is tested and is replaced by 1 when the divisor is <= 0. On Xenon
# the division of the rejected case does not trap.
.globl fn_guarded_quotient
fn_guarded_quotient:
    divw r11,r3,r4
    cmpwi cr6,r4,0
    bgt cr6,.Lguarded_keep
    li r11,1
.Lguarded_keep:
    mr r3,r11
    blr
