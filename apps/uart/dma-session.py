#!/usr/bin/env python3
"""Produce a sealed UART DMA session template from caller bytes and authority.

The base session supplies existing board/UART evidence. Its initial authority
must already name the exact new input. This producer establishes no physical
prerequisites; preparation separately authenticates every referenced fact.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import json
import struct
spec=importlib.util.spec_from_file_location('uart_dma_prepare',Path(__file__).with_name('prepare.py'))
prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
def produce(base,data,authority):
    prepare.session_check(base)
    prepare.require(1<=len(data)<=1024,'UART DMA input must contain1..1024bytes.')
    record=json.loads(authority)
    prepare.require(record.get('schema')=='a733-uart-dma-authority-v1','Wrong DMA authority schema.')
    config=bytes.fromhex(record['configuration_hex'])
    prepare.require(len(config)==96,'DMA configuration must contain96bytes.')
    out=bytearray(base[:800])+bytearray(data)+bytearray(1024-len(data))+bytearray(config)+bytearray(hashlib.sha256(authority).digest())
    struct.pack_into('<2Q',out,600,2,1952);struct.pack_into('<2I',out,792,len(data),len(data))
    prepare.session_check(out);return bytes(out)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('base-session','input','dma-authority','output'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();a.output.write_bytes(produce(a.base_session.read_bytes(),a.input.read_bytes(),a.dma_authority.read_bytes()))
