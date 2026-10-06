# Original regression for generator patch 0022: the providing module (a DLL other
# modules import by ordinal). Built at its own image base. No game bytes.
#_ XEX_ENTRY provider_entry

.text
.globl provider_entry
provider_entry:
    li r3, 1
    blr

# r3 = r3 + r4 (the export the consumer imports as Provider.dll ordinal 1).
.globl provider_add
provider_add:
    add r3, r3, r4
    blr
