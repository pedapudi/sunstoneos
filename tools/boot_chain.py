"""Watchdog recovery of each stage of the card boot chain, judged only by what the board has shown.

The card boot under test control (docs/designs/card-fel-recovery.md) returns to FEL only if WDT0 resets the board
whenever a stage hangs or stops. Register readback cannot show that: on 2026-11-13 the first boot-once run
(1794619098327285673-8466a823) passed every readback in the host model and never returned. This module therefore
grants recovery to a stage only when the WDT0 arming that covers it, and the state in which it would expire, match
an arming the board has been seen to expire from. docs/designs/card-boot-chain.md gives the reasoning.

checks/card-boot-chain.py derives each stage's arming from the gate and stage-2 code (it runs them) and fails when it
differs from STAGES. tools/board_images.py boot_once_slot() refuses a boot-once run while unproven() lists a stage.
"""

# A WDT0 mode write that enables the watchdog is an arming. 'from-0' means WDT0 read disarmed (mode bit 0 clear)
# before it; 'rearm' means it was already enabled, as after a watchdog reset, where WDT0 still reads the mode armed
# before the reset (card-fel-recovery step E).
FROM_ZERO, REARM = 'from-0', 'rearm'

# Where the arming would expire. 'fel': in a session that FEL launched, with no boot0 in this power epoch.
# 'after-boot0': after the vendor boot0 has trained DRAM and written the PMIC (DCDC2, DCDC4, DCDC6, DCDC7, ELDO1).
FEL, AFTER_BOOT0 = 'fel', 'after-boot0'

# (arming, mode, where it expires) -> board records of an expiry that reset the board into FEL.
MEASURED = {
    (FROM_ZERO, 0x41, FEL): ('1794503842447280399-ba6865c9: the runner armed 0x41 from mode 0 and the reset came back '
                             'through the card gate (warm record 0x0c024041); every later session reset repeats it',),
    (FROM_ZERO, 0xb1, FEL): ('1794503769456522126-00c48b8a: the rail-restore helper armed 0xb1 from mode 0 and its '
                             'reset came back through the card gate (warm record 0x0c0240b1)',),
}

# Expiry states with no board measurement and no FEL-only test. Each stays open until a board record is added to
# MEASURED or the coordinator records an acceptance in ACCEPTED; neither happens by default.
OPEN_RISKS = {
    AFTER_BOOT0: 'no board run has let WDT0 expire after boot0 trained DRAM and wrote the PMIC. Vendor U-Boot reboots '
                 'through WDT0 after the same boot0, which is a reference, not a measurement, and no FEL-only test can '
                 'reach that state.',
}

# Re-arms over an enabled WDT0 that the board has shown to expire. Both ran from FEL, over a 0x41 that the runner had
# armed from mode 0 in the same power epoch; the mode write restarted the counter at the new interval.
REARM_MEASURED = (
    '1794643227744371298-6101ee0d: the runner wrote 0xb1 over its running 0x41 with no disarm, and the reset came '
    '18.137 s later',
    '1794643284421551583-789b13b5: the gate warm path, executed from SRAM, wrote 0xb1 over the runner\'s running 0x41 '
    'with no disarm, and the reset came 18.169 s after the execute request',
)

# The re-arm conditions of the 2026-11-13 boot-once run that REARM_MEASURED does not cover. The gate and stage 2 now
# disarm before every arming, so no stage depends on them; a re-arm stays unproven while any is listed.
REARM_OPEN = (
    'a re-arm over a mode that survived its own expiry reset (WDT0 still reads the mode armed before a watchdog reset, '
    'card-fel-recovery step E)',
    'output and configuration writes over a running counter, which the gate cold path made before its mode write',
)

# where -> {'by': ..., 'date': ..., 'reference': ...}. An accepted risk grants recovery for an arming that the board
# has shown to expire in a FEL session. Only the coordinator adds an entry, with its written reference.
ACCEPTED = {}

# The boot-once chain after the gate's cold path, in order. 'arming' is what checks/card-boot-chain.py derives from
# the code; 'source' names the code that arms.
STAGES = (
    {'name': 'boot0', 'covers': 'boot0: DRAM training, PMIC writes, the TOC1 load and the monitor entry',
     'source': 'boot/a733/card-fel-gate.S cold path', 'arming': FROM_ZERO, 'mode': 0xb1, 'expires': AFTER_BOOT0},
    {'name': 'stage2', 'covers': 'stage 2: card open, slot headers and slot load (restarted after each card read)',
     'source': 'boot/a733/card-fel-gate.S cold path', 'arming': FROM_ZERO, 'mode': 0xb1, 'expires': AFTER_BOOT0},
    {'name': 'os', 'covers': 'the OS from its entry to its stop, which leaves the reset to WDT0',
     'source': 'boot/a733/stage2/loader.c arm()', 'arming': FROM_ZERO, 'mode': 0x41, 'expires': AFTER_BOOT0},
)

WDT_MODE = 0x02050014


def armings(writes, mode):
    """The armings in a sequence of (address, value) register writes, given WDT0's mode before the first one.

    Only keyed mode writes take effect (A733 manual 8.3.6.6); each enabling write is an arming, classified by whether
    WDT0 read disarmed before it."""
    found = []
    for address, value in writes:
        if address != WDT_MODE or value >> 16 != 0x16aa:
            continue
        new = value & 0xffff
        if new & 1:
            found.append((REARM if mode & 1 else FROM_ZERO, new))
        mode = new
    return found


def recovery(stage, measured=None, accepted=None):
    """The board record that grants this stage recovery, or None."""
    measured = MEASURED if measured is None else measured
    accepted = ACCEPTED if accepted is None else accepted
    key = (stage['arming'], stage['mode'], stage['expires'])
    if key in measured:
        return measured[key]
    if stage['expires'] in accepted and (stage['arming'], stage['mode'], FEL) in measured:
        return measured[(stage['arming'], stage['mode'], FEL)] + (accepted[stage['expires']],)
    return None


def unproven(stages=STAGES, measured=None, accepted=None):
    """One sentence for each stage without a measured or accepted recovery; empty when every stage has one."""
    problems = []
    for stage in stages:
        if recovery(stage, measured, accepted) is not None:
            continue
        why = OPEN_RISKS.get(stage['expires'], 'no board record shows this arming expire')
        if stage['arming'] == REARM and REARM_OPEN:
            why += '; a re-arm is unmeasured for ' + ' and for '.join(REARM_OPEN)
        problems.append('%s (%s): WDT0 %s to 0x%x, expiring %s, has no measured recovery; %s' % (
            stage['name'], stage['covers'], 'armed from mode 0' if stage['arming'] == FROM_ZERO else 're-armed without a disarm',
            stage['mode'], stage['expires'], why))
    return problems
