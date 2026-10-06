# Original PPC fixture, no title content. The second function conditionally
# tail-calls the earlier one. Exercise both paths after real recompilation.
.text
# Keep the target away from address zero (the generator's unset longjmp
# address) so this fixture tests ordinary function dispatch.
.space 4
.global tail_target
tail_target:
.Ltail_target:
    li r3, 77
    blr
.global tail_entry
tail_entry:
    cmpwi r3, 0
    beq .Ltail_target
    li r3, 11
    blr
