# Original regression for generator patch 0023: what must stay reported. No
# game bytes. The doubleword signed division record forms had no CR0 update
# before patch 0023 and keep none (patch 0023 changes no CR side effect), so
# the generator still reports them ("RC bit enabled but no comparison was
# generated", a blocking generator diagnostic for the inventory).
#_ XEX_ENTRY _start

.text
.globl _start
_start:
    li r3,0
    blr

.globl fn_divd_rc
fn_divd_rc:
    divd. r3,r3,r4
    blr

.globl fn_divdo_rc
fn_divdo_rc:
    divdo. r3,r3,r4
    blr
