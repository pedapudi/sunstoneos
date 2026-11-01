# Validate CPU command spans before constructing display work

The G2D command builder accepts a 4096-byte output page and a session that
describes its geometry. The display-engine ring helpers accept two to four
4096-byte command pages. These CPU spans need validation independently of the
device addresses written into the commands.

G2D must reject a session that overlaps its output page. Otherwise clearing
the page changes the session after geometry validation and before command
construction. Both session and output spans must have representable exclusive
ends before the builder reads the session or writes commands.

The display ring builder and validator must reject a wrapping CPU span before
clearing or reading any page. Their existing device-bank validation establishes
the slot count before multiplication by the page size. Device geometry alone
does not establish a valid CPU span.

These checks run once per command-bank construction or validation. G2D adds
one range-pair predicate; each display-ring entry adds one span predicate.
Accepted calls keep identical command bytes and the same copies and clears.
Refused calls leave all accessible input and output bytes unchanged. No new
allocation, translation, lock, barrier, register access or scheduler wakeup is
needed. There is no change to clocks, voltage or power-domain control.

The host check, `checks/display-command-admission.py`, tests G2D with adjacent
and overlapping sessions. Both builders receive wrapping spans under address
and undefined-behavior sanitizers. The check compares accepted
G2D pages with the pre-change builder across every admitted operation and RGB
order. It also checks each accepted display-ring slot count and the legacy
two-slot wrappers. Pre-change negative cases reproduce G2D session corruption
and the ring builder's invalid memory access. Physical pixels, interrupts and buffer retirement remain separate
qualification requirements.
