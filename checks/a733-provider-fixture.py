#!/usr/bin/env python3
"""Fixed PMIC/USB provider data for software-only workload-2 fixtures.

The pinned accepted thermal fixture supplies configuration, never physical
ownership or board authority. Application producers retain their own image,
evidence, session identity, owners and extension when copying these fields.
"""
import hashlib
from pathlib import Path
import struct

SOURCE=Path.home()/'.cache/sunstoneos/evidence/thermal-single-core-20261018/MODEL-session.bin'
SOURCE_SHA256='da5ac5affd41a12e7265df34aefcb874bcbe060cdc7ddfcd006dbe15bf451502'
PROVIDER_WORDS=(0,0x80000000,0x80000000,24000000,24000000,24000000,
                3300000,3300000,1200000,900000,0,2,0,0,0,0,0,0,0)
PMIC_PINS=bytes((2,1,1,2,1,1))

def require(condition,message):
    if not condition:raise ValueError(message)

def board_prefix_check(raw):
    require(len(raw)>=592 and struct.unpack_from('<3Q',raw)==(0x4153373342535332,2,592),
            'Nominal provider fixture requires the supervised 592-byte board ABI.')
    require(struct.unpack_from('<2I',raw,400)==(2,0),'Nominal providers require workload 2 without reserved flags.')
    pmic,usb=struct.unpack_from('<2Q',raw,456)
    require(pmic and usb and pmic!=usb,'Nominal providers require distinct PMIC and USB owners.')

def source_bytes():
    raw=SOURCE.read_bytes()
    require(len(raw)==808 and hashlib.sha256(raw).hexdigest()==SOURCE_SHA256,
            'Accepted software-only provider fixture source differs from its pin.')
    board_prefix_check(raw)
    require(struct.unpack_from('<19I',raw,488)==PROVIDER_WORDS and raw[564:570]==PMIC_PINS,
            'Accepted software-only provider fixture configuration differs.')
    return raw

def validate_nominal_providers(raw,usb_reference_mode=2):
    """Reject incomplete fixed provider setup before any instruction emulation."""
    require(usb_reference_mode in (1,2),'Nominal USB mode must be cold reference or ROM takeover.')
    board_prefix_check(raw);source=source_bytes()
    expected=bytearray(source[488:570]);struct.pack_into('<I',expected,532-488,usb_reference_mode)
    require(raw[488:570]==expected,
            'Nominal PMIC/USB profile is incomplete: check fabric enables/rates, supplies, USB mode and PL0/PL1 pins.')

def nominal_provider_session(raw,usb_reference_mode=2):
    """Copy only fixed provider fields; preserve all application authority bytes."""
    board_prefix_check(raw);source=source_bytes();result=bytearray(raw)
    result[488:570]=source[488:570]
    struct.pack_into('<I',result,532,usb_reference_mode)
    validate_nominal_providers(result,usb_reference_mode)
    return result
