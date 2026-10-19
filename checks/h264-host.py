#!/usr/bin/env python3
"""Exercise the new H.264 host admission/capture boundary without devices."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('batch', type=Path)
    a = p.parse_args(); out = a.batch; source = out/'source'
    capture = module('h264_host_capture', source/'tools/check-a733-h264-supervised.py')
    prepare = capture.preparation
    raw = bytearray((out/'session.bin').read_bytes())
    raw[768:800] = hashlib.sha256((out/'slice.bin').read_bytes()).digest()
    raw[800:832] = hashlib.sha256((out/'reference.yuv').read_bytes()).digest()
    frame = (out/'native-frame.bin').read_bytes()
    pending = bytearray(frame[:128]); struct.pack_into('<2I', pending, 8, 0, 0); pending[96:128] = bytes(32)
    evidence = out/'host'; evidence.mkdir(exist_ok=False)
    for name in ('nominal', 'wrong-session', 'wrong-pixels', 'deadline'):
        folder = evidence/name; folder.mkdir(); record = {}; clock = [0.0]; calls = [0]
        def save():
            (folder/'record.json').write_text(json.dumps(record, indent=2)+'\n')
        def exchange(packet, timeout):
            assert packet == bytes(1) and 0 < timeout <= 500
            calls[0] += 1
            if calls[0] == 1: return bytes(pending)
            value = bytearray(frame)
            if name == 'wrong-session': struct.pack_into('<Q', value, 16, 99)
            if name == 'wrong-pixels': value[128] ^= 1
            if name == 'deadline': clock[0] = 10
            return bytes(value)
        try:
            result = capture.capture(exchange, bytes(raw), folder, record, save, 5,
                                     now=lambda: clock[0], pause=lambda seconds: None,reference=(out/'reference.yuv').read_bytes())
        except (ValueError, RuntimeError):
            assert name != 'nominal' and record['h264_result'] == 'unobserved'
            assert (folder/'h264-002.bin').exists()  # Refused raw bytes remain durable.
        else:
            assert name == 'nominal' and result['copied_verified']
            assert (folder/'output.yu12').read_bytes() == (out/'reference.yuv').read_bytes()
    # Only the older common board authority is modeled here. Actual H.264 fact
    # decoding, target binding, six ranges and immutable evidence hashes run.
    schemas = {
        'initial': ['all_masters_excluded','prior_transactions_drained','prior_walks_drained','decoder_off_reset_held',
            'skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq81_owned_masked','iommu_irqs_owned_masked'],
        'memory': ['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','immutable_rom_mapping',
            'guard_pages_unmapped','source_extents_reviewed'],
        'electrical': ['dcdc2_shared_800mv','ve_operating_point_qualified','stable_ve_pll'],
        'supervision': ['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}
    facts = {key: dict(schema='a733-h264-'+key+'-v1', identity='modeled-target', port='modeled-port',
                      image_sha256='12'*32, reviewed=True, **{name: True for name in flags}) for key, flags in schemas.items()}
    facts['memory'].update(iova_spans=[[0x10001000,4096,'r'],[0x10010000,4096,'w'],[0x10014000,4096,'w'],
        [0x10020000,0x21000,'rw'],[0x10044000,4096,'rw'],[0x10048000,0xc000,'rw']], decoder_masters=[6,7])
    saved = prepare.board.validate_admission
    prepare.board.validate_admission = lambda record, image, template, cache, frozen: cache
    try:
        for name in ('nominal', 'overlap', 'wrong-access', 'one-master', 'no-guards', 'reusable'):
            values = copy.deepcopy(facts)
            if name == 'overlap': values['memory']['iova_spans'][4][0] = 0x10040000
            if name == 'wrong-access': values['memory']['iova_spans'][0][2] = 'rw'
            if name == 'one-master': values['memory']['decoder_masters'] = [6]
            if name == 'no-guards': values['memory']['guard_pages_unmapped'] = False
            if name == 'reusable': values['supervision']['no_reuse'] = False
            session = bytearray(raw); cache = {}; record = dict(schema='a733-h264-supervised-entry-v1', identity='modeled-target', port='modeled-port')
            for key, offset in zip(schemas, (640,672,704,736)):
                data = json.dumps(values[key], sort_keys=True).encode(); path = str((evidence/(name+'-'+key+'.json')).resolve())
                Path(path).write_bytes(data); digest = hashlib.sha256(data).digest(); cache[path] = data
                record[key] = {'path': path, 'sha256': digest.hex()}; session[offset:offset+32] = digest
            try: prepare.validate_admission(record, '12'*32, '34'*32, bytes(session), cache, True)
            except (ValueError, RuntimeError): assert name != 'nominal'
            else: assert name == 'nominal'
    finally: prepare.board.validate_admission = saved
    (evidence/'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'scope': 'Actual host decoding/capture and new six-span authority checks; common physical authority and transport are modeled.'}, indent=2)+'\n')
    print('PASS: H.264 durable host capture/refusal and six-span admission.')


if __name__ == '__main__': main()
