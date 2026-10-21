#!/usr/bin/env python3
"""Check that launch preparation accepts the commitment table the update writer seals.

tools/storage-update.py writes one 32-byte commitment per WRITE_CHUNK of the
candidate. tools/check-a733-storage-launch.py reopens that table with an exact
size before it binds the grant into a session. This check runs the actual
writer preparation for one, two and three sectors and then the actual launch
input validation over the sealed store. No board, USB or card access.
"""
import argparse
import importlib.util
import os
from pathlib import Path
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('storage_launch_chain',ROOT/'tools/check-a733-storage-launch.py')
    launch=importlib.util.module_from_spec(spec);spec.loader.exec_module(launch)
    api=launch.store_api
    for blocks in (1,2,3):
        case=out/f'blocks-{blocks}';case.mkdir()
        size=blocks*512
        grant=dict(mode=1,kind=1,host=0,nonce=bytes(range(1,33)).hex(),cid=[1,2,3,4],sectors=1024,first=128,
                   blocks=blocks,deadline_us=30000000,backup='00'*32,candidate='00'*32,allocation='45'*32,recovery='67'*32)
        backup=bytes((i*7+blocks)&255 for i in range(size))
        candidate=case/'candidate-source.bin';candidate.write_bytes(bytes((i*13+1)&255 for i in range(size)))
        with api.ProtectedDirectory(case/'original',create=True) as original:
            fd=original.create('backup.bin');api.write_all(fd,backup);original.seal('backup.bin',fd,size,api.digest(backup))
            original.record('backup.json',dict(grant=dict(grant,mode=0),bytes=size,sha256=api.digest(backup).hex()))
            with api.ProtectedDirectory(case/'prepared',create=True) as store:
                manifest=api._prepare_files(store,original,candidate,grant)
        assert os.stat(case/'prepared/chain.bin').st_size==blocks*512//api.WRITE_CHUNK*32
        with api.ProtectedDirectory(case/'prepared') as store:
            raw,decoded,identity=launch.storage_inputs(store)
        assert decoded['mode']==1 and decoded['blocks']==blocks and raw==(case/'prepared/grant.bin').read_bytes()
        assert identity==launch.sha(api.canonical(manifest))
    print('PASS: launch preparation accepts the sealed one-commitment-per-chunk table for 1, 2 and 3 sectors')


if __name__=='__main__':main()
