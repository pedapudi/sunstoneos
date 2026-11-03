# Validate CPU command spans before constructing display work

Status: implemented and host-tested. Physical pixels, interrupts and buffer
retirement are separate qualification requirements.

The G2D command builder accepts a 4096-byte output page and a session that
describes its geometry. The display-engine ring helpers accept two to four
4096-byte command pages. These CPU spans are validated independently of the
device addresses written into the commands.

G2D rejects a session that overlaps its output page (`a733_ranges_overlap_or_wrap`
in `drivers/g2d/a733_g2d.c`); otherwise clearing the page would change the
session after geometry validation and before command construction. Session and
output spans must have representable exclusive ends before the builder reads
the session or writes commands.

The display ring builder and validator reject a wrapping CPU span before
clearing or reading any page. Device-bank validation establishes the slot count
before multiplication by the page size, but device geometry alone does not
establish a valid CPU span.

The checks run once per command-bank construction or validation: G2D adds one
range-pair predicate, and each display-ring entry adds one span predicate.
Accepted calls produce identical command bytes with the same copies and clears.
Refused calls leave all accessible input and output bytes unchanged. The change
adds no allocation, translation, lock, barrier, register access or scheduler
wakeup, and no clock, voltage or power-domain control.

## Checks

`checks/display-command-admission.py` tests G2D with adjacent and overlapping
sessions, and both builders with wrapping spans under address and
undefined-behavior sanitizers. It compares accepted G2D pages with the earlier
builder across every admitted operation and RGB order, and checks each accepted
display-ring slot count and the two-slot wrappers. The result is in
[display-command-admission.jsonl](../evidence/display-command-admission.jsonl)
(`display-command-admission-host`).
