#!/usr/bin/env python3
"""Session fields, radio profile and firmware uploads for the scanning radio image.

The image (SUNSTONE_A733_OS_RADIO_SCAN) is a returning OS image whose radio station scans and shuts the module down.
It needs a board session with workload 3 (radio, no USB host), a fixed 1,872-byte scan-only radio profile (version 8,
the supervised upload form of version 7) at 0x43000000, and the four firmware images the profile names, in the
retained range from 0x43010000. The profile carries no WPA2 target, entropy or TCP exchange.

Every offset below is checked against the compiled C layout by checks/radio-scan-profile.py.
"""
import hashlib
import os
import struct
from pathlib import Path

PROFILE_ADDRESS = 0x43000000
PROFILE_BYTES = 1872
FIRMWARE_ADDRESS = 0x43010000
FIRMWARE_STRIDE = 0x10000
FIRMWARE_DIR = Path.home() / '.cache/sunstoneos/sources/radio/firmware/aic8800d80'
# The non-H U02 set the pinned overlay carries (docs/radio-firmware.md); sizes and hashes are the native catalogue's.
FIRMWARE = [('fw_adid_8800d80_u02.bin', 1708, 'a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56'),
            ('fw_patch_8800d80_u02.bin', 24744, '4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0'),
            ('fw_patch_table_8800d80_u02.bin', 23288, '5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60'),
            ('fmacfw_8800d80_u02.bin', 341212, '55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c')]
SCAN_SUPERVISED_VERSION = 8
# How far the session runs (BOARD_RADIO_STAGE_* in apps/board/radio.h): 0 a full run (the scan), 1 enumeration, 2
# enumeration plus the chip-ID words, 3 through the firmware upload, 4 the CE entropy stage alone (the module stays
# unpowered), 5 association with the stored network and the four-way handshake (unqualified CE entropy, bring-up only). It is a submit-time option (board.py submit --radio-stage-limit N), frozen in the queue entry, so a new stage
# needs no landing of this file.
STAGE_LIMITS = (0, 1, 2, 3, 4, 5)
ASSOCIATE_STAGE = 5
STAGE_LIMIT_OFFSET = 1861
ENTROPY_UNQUALIFIED_OFFSET = 1862   # network_reserved[1]: 1 on the association stage, else 0
# The identity the profile expects (raw words, decoded again by the firmware engine): the words the board's module
# returned at the identity stage (run 1793750775070389039-2a278fd8, port 3-1): chip word 0xfb078820, revision 7 (U03,
# which the vendor drives with the u02 firmware set, aic_bsp_8800d80.c:676-690), not an H part, subrevision word 0x821.
IDENTITY = {'chip_word': 0xfb078820, 'subrevision_word': 0x821}
# Offsets of struct board_radio_profile (apps/board/radio.h) and its members.
PROFILE = {'magic': 0, 'version': 8, 'bytes': 12, 'platform_authority': 16, 'platform_evidence': 88, 'platform': 184,
           'radio_identity': 496, 'transport': 512, 'bootstrap': 544, 'wifi': 568, 'firmware_address': 432,
           'firmware_bytes': 464, 'session_owner': 1224, 'runtime_lease_us': 1240, 'renew_margin_us': 1248,
           'binding_timeout_us': 1256, 'command_timeout_us': 1264, 'refill_timeout_us': 1272, 'association_timeout_us': 1296,
           'security_timeout_us': 1304}
PLATFORM = {'owner': 0, 'topology': 32, 'fabric': 56, 'fabric_hz': 68, 'expected': 80, 'desired': 104, 'card': 128}
# Offsets of struct a733_board_session (include/a733_board_application.h).
SESSION = {'topology': 184, 'sid_authority': 312, 'sid_evidence': 320, 'radio_profile_address': 352,
           'radio_profile_bytes': 360, 'radio_profile_digest': 368, 'workload': 400, 'latch_m_identity': 432,
           'latch_m_owner': 440, 'latch_m_known_mask': 448, 'latch_m_value': 452, 'pmic_owner': 456,
           'usb0_owner': 464, 'fabric': 488, 'fabric_hz': 500}
RAIL = {'dcdc1': 0, 'bldo5': 2, 'cldo1': 3}   # enum a733_radio_rail
PLATFORM_OWNER, SESSION_OWNER, PM_OWNER = 0x5241444f5053, 0x5241444f5353, 0x5241444f504d


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def channels():
    """Passive-scan channels: 2.4 GHz channels 1 to 11 and 5 GHz channels 36 to 48. NO_IR on every one, no DFS channel."""
    plan = [(2407 + 5 * n, 0, 17) for n in range(1, 12)] + [(5000 + 5 * n, 1, 10) for n in (36, 40, 44, 48)]
    return [(freq, band, 1, power) for freq, band, power in plan]


def firmware_files(directory=None):
    """The four verified firmware images: [(address, name, data)]."""
    directory = Path(directory or FIRMWARE_DIR)
    result = []
    for index, (name, size, digest) in enumerate(FIRMWARE):
        data = (directory / name).read_bytes()
        require(len(data) == size and hashlib.sha256(data).hexdigest() == digest, 'Firmware image differs from the pinned catalogue: ' + name)
        result.append((FIRMWARE_ADDRESS + FIRMWARE_STRIDE * index, name, data))
    return result


def check_stage_limit(limit):
    """The limit as an int; ValueError for a value the profile does not define."""
    if isinstance(limit, bool) or not isinstance(limit, int) or limit not in STAGE_LIMITS:
        raise ValueError('radio stage limit %r is not one of %s' % (limit, STAGE_LIMITS))
    return limit


def profile_bytes(prefix, evidence, firmware, stage_limit=0):
    """The 1,872-byte scan-only profile for this board session prefix. evidence is 32 bytes naming the reviewed
    reference; it fills the platform evidence and authority fields, which only have to be nonzero."""
    require(len(evidence) == 32 and any(evidence), 'Evidence digest required.')
    raw = bytearray(PROFILE_BYTES)
    struct.pack_into('<QII', raw, 0, 0x3152464f52545353, SCAN_SUPERVISED_VERSION, PROFILE_BYTES)
    struct.pack_into('<Q', raw, PROFILE['platform_authority'], int.from_bytes(evidence[:8], 'little') | 1)
    raw[PROFILE['platform_evidence']:PROFILE['platform_evidence'] + 32] = evidence
    p = PROFILE['platform']
    topology = struct.unpack_from('<Q', prefix, SESSION['topology'])[0]
    struct.pack_into('<Q', raw, p + PLATFORM['owner'], PLATFORM_OWNER)
    # Vendor topology: VBAT is DCDC1, PG is BLDO5, PM is CLDO1, module VDDIO is BLDO5.
    struct.pack_into('<QIIII', raw, p + PLATFORM['topology'], topology, RAIL['dcdc1'], RAIL['bldo5'], RAIL['cldo1'], RAIL['bldo5'])
    raw[p + PLATFORM['fabric']:p + PLATFORM['fabric'] + 12] = prefix[SESSION['fabric']:SESSION['fabric'] + 12]
    raw[p + PLATFORM['fabric_hz']:p + PLATFORM['fabric_hz'] + 12] = prefix[SESSION['fabric_hz']:SESSION['fabric_hz'] + 12]
    # PG0..PG5 then PM0, PM1: inherited disabled with the reset pull (off) and drive (level 1); desired SDC1 function 2
    # with pull-up (drive level 1 on PG0, the clock, level 0 on the rest, as the vendor pin state: 20 and 10 mA), PM0
    # input and PM1 output.
    for i in range(8):
        struct.pack_into('<3B', raw, p + PLATFORM['expected'] + 3 * i, 15, 0, 1)
        function, pull, drive = (2, 1, 1 if i == 0 else 0) if i < 6 else (0, 0, 1) if i == 6 else (1, 0, 1)
        struct.pack_into('<3B', raw, p + PLATFORM['desired'] + 3 * i, function, pull, drive)
    struct.pack_into('<I4xQQ', raw, p + PLATFORM['card'], 0x00300000, 100000, 1000)  # supported OCR, then timeout and poll interval
    raw[STAGE_LIMIT_OFFSET] = check_stage_limit(stage_limit)
    struct.pack_into('<II4B', raw, PROFILE['radio_identity'], IDENTITY['chip_word'], IDENTITY['subrevision_word'],
                     (IDENTITY['chip_word'] >> 16) & 0x3f, IDENTITY['subrevision_word'] >> 4, 0, 1)
    # Transport: D80 bootstrap variant 1, pad policy KEEP (1), 1,000 ticks per millisecond, timeouts in microseconds.
    struct.pack_into('<IIQQQ', raw, PROFILE['transport'], 1, 1, 1000, 100000, 1000)
    struct.pack_into('<QIIBB', raw, PROFILE['bootstrap'], int.from_bytes(evidence[8:16], 'little') | 1, 0x40500058, 0x40, 1, 1)
    w = PROFILE['wifi']
    plan = channels()
    struct.pack_into('<QQQII', raw, w, int.from_bytes(evidence[16:24], 'little') | 1,
                     int.from_bytes(evidence[24:32], 'little') | 1, 500000, 1, len(plan))
    for i, (freq, band, flags, power) in enumerate(plan):
        struct.pack_into('<HBBbx', raw, w + 32 + 6 * i, freq, band, flags, power)
    for i, (address, _, data) in enumerate(firmware):
        struct.pack_into('<Q', raw, PROFILE['firmware_address'] + 8 * i, address)
        struct.pack_into('<Q', raw, PROFILE['firmware_bytes'] + 8 * i, len(data))
    struct.pack_into('<Q', raw, PROFILE['session_owner'], SESSION_OWNER)
    struct.pack_into('<QQ', raw, PROFILE['runtime_lease_us'], 20000000, 5000000)
    struct.pack_into('<QQ', raw, PROFILE['binding_timeout_us'], 5000000, 1000000)
    if stage_limit == ASSOCIATE_STAGE:
        # The time budget against the 50 s startup deadline (firmware confirmed near 16.7 s of session time): scan 10 s,
        # authentication and association 8 s, four-way handshake 8 s, 2 s of supplicant command bound at 1 s; the stage stops
        # at the session deadline whichever is first. The entropy is UNQUALIFIED (bring-up only): byte 1 declares it.
        raw[ENTROPY_UNQUALIFIED_OFFSET] = 1
        struct.pack_into('<Q', raw, PROFILE['command_timeout_us'], 1000000)
        struct.pack_into('<Q', raw, PROFILE['refill_timeout_us'], 8000000)     # authentication and association
        struct.pack_into('<Q', raw, PROFILE['association_timeout_us'], 10000000)   # bounds the scan
        struct.pack_into('<Q', raw, PROFILE['security_timeout_us'], 8000000)   # the four-way handshake
        return bytes(raw)
    struct.pack_into('<Q', raw, PROFILE['association_timeout_us'], 20000000)   # bounds the scan
    return bytes(raw)


def apply(prefix, evidence, firmware_dir=None, stage_limit=0):
    """Patch a 592-byte board session prefix for the scanning radio image. Returns (profile, uploads) where uploads are
    [(address, name, data)] for the profile and the four firmware images."""
    require(len(prefix) == 592, 'Board prefix size differs.')
    firmware = firmware_files(firmware_dir)
    profile = profile_bytes(prefix, evidence, firmware, stage_limit)
    struct.pack_into('<I', prefix, SESSION['workload'], 3)
    struct.pack_into('<Q', prefix, SESSION['sid_authority'], int.from_bytes(evidence[:8], 'little') | 1)
    prefix[SESSION['sid_evidence']:SESSION['sid_evidence'] + 32] = evidence
    struct.pack_into('<QQ', prefix, SESSION['radio_profile_address'], PROFILE_ADDRESS, PROFILE_BYTES)
    prefix[SESSION['radio_profile_digest']:SESSION['radio_profile_digest'] + 32] = hashlib.sha256(profile).digest()
    # The PM latch record: nonzero identity and owner with an empty known mask, the normal OS ownership form.
    struct.pack_into('<QQII', prefix, SESSION['latch_m_identity'], int.from_bytes(evidence[:8], 'little') | 1, PM_OWNER, 0, 0)
    uploads = [(PROFILE_ADDRESS, 'radio-profile.bin', profile)]
    uploads += [(address, 'radio-' + name, data) for address, name, data in firmware]
    return profile, uploads
