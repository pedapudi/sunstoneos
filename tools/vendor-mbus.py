"""The MBUS word and rate that a session's fabric record names.

Every EL3 entry restores MBUS from the boot ROM's 400 MHz (0x84000000,
PERI0_400M) to boot0's 600 MHz (0x81000000, PERI0_600M undivided)
(include/a733_el3_mbus.h, docs/designs/mbus-restore.md). Every driver that
checks the bus fabric compares the live word with its session, so a session
that names the ROM's word and rate is refused by an image built with the
restore. Session producers pass their fabric record through these functions:
the ROM's word and rate become boot0's; any other record is returned
unchanged, because the restore leaves any other register value alone and
software-only fixtures model their own fabric.
"""
import struct

WORD, HZ = 0x81000000, 600000000
ROM_WORD, ROM_HZ = 0x84000000, 400000000
# struct a733_board_session (include/a733_board_application.h): fabric[3]
# at byte 488 and fabric_hz[3] at byte 500; MBUS is the third of each.
BOARD_FABRIC, BOARD_FABRIC_HZ, BOARD_BYTES = 488, 500, 592


def fabric(words, rates):
    """The AHB, NSI and MBUS words and rates as the restored image finds them."""
    words, rates = list(words), list(rates)
    if len(words) != 3 or len(rates) != 3:
        raise ValueError('A fabric record has three words and three rates.')
    if (words[2], rates[2]) == (ROM_WORD, ROM_HZ):
        words[2], rates[2] = WORD, HZ
    return words, rates


def board_session(raw):
    """A copy of a board session (592 bytes, or a longer session that starts
    with one) whose fabric record names MBUS as the restored image finds it."""
    if len(raw) < BOARD_BYTES:
        raise ValueError('A board session has at least 592 bytes.')
    out = bytearray(raw)
    words = struct.unpack_from('<3I', out, BOARD_FABRIC)
    rates = struct.unpack_from('<3I', out, BOARD_FABRIC_HZ)
    words, rates = fabric(words, rates)
    struct.pack_into('<3I', out, BOARD_FABRIC, *words)
    struct.pack_into('<3I', out, BOARD_FABRIC_HZ, *rates)
    return bytes(out)
