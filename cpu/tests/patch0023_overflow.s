# Original regression for generator patch 0023: the overflow-enable (OE) forms
# of the integer divisions, which a generator without patch 0023 reports as
# unrecognized. No game bytes. Inputs: r3 = dividend, r4 = divisor; result in
# r3; XER[OV]/XER[SO] and CR0 are read from the context by the test.
#_ XEX_ENTRY _start

.text
.globl _start
_start:
    li r3,0
    blr

.globl fn_divwo
fn_divwo:
    divwo r3,r3,r4
    blr

.globl fn_divwuo
fn_divwuo:
    divwuo r3,r3,r4
    blr

.globl fn_divdo
fn_divdo:
    divdo r3,r3,r4
    blr

.globl fn_divduo
fn_divduo:
    divduo r3,r3,r4
    blr

# Record + OE: CR0[SO] is the XER[SO] this instruction updated.
.globl fn_divwo_rc
fn_divwo_rc:
    divwo. r3,r3,r4
    blr

.globl fn_divwuo_rc
fn_divwuo_rc:
    divwuo. r3,r3,r4
    blr

.globl fn_divduo_rc
fn_divduo_rc:
    divduo. r3,r3,r4
    blr

# rD == rB: XER[OV] must come from the old r4 (INT_MIN / -1 overflows; the
# quotient INT_MIN written to r4 first would not).
.globl fn_divwo_alias
fn_divwo_alias:
    divwo r4,r3,r4
    mr r3,r4
    blr
