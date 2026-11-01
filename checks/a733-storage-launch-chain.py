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
        allocation=dict(owner='SunstoneOS',card_range={k:grant[k] for k in ('kind','host','cid','sectors','first','blocks')})
        grant['allocation']=api.digest(api.canonical(allocation)).hex()
        backup=bytes((i*7+blocks)&255 for i in range(size))
        candidate=case/'candidate-source.bin';candidate.write_bytes(bytes((i*13+1)&255 for i in range(size)))
        with api.ProtectedDirectory(case/'original',create=True) as original:
            fd=original.create('backup.bin');api.write_all(fd,backup);original.seal('backup.bin',fd,size,api.digest(backup))
            original.record('backup.json',dict(grant=dict(grant,mode=0,nonce='02'*32),bytes=size,sha256=api.digest(backup).hex()))
            with api.ProtectedDirectory(case/'prepared',create=True) as store:
                try:api.prepare(store,original,candidate,grant,None,allocation)
                except FileNotFoundError as error:assert 'backup-complete.json' in str(error)
                else:raise AssertionError('Update prepared without a complete backup')
                assert not list(store.path.iterdir())
                original.record('backup-complete.json',dict(native_sha256=api.digest(backup).hex()))
                manifest=api.prepare(store,original,candidate,grant,None,allocation)
        assert os.stat(case/'prepared/chain.bin').st_size==blocks*512//api.WRITE_CHUNK*32
        with api.ProtectedDirectory(case/'prepared') as store:
            raw,decoded,identity=launch.storage_inputs(store)
        assert decoded['mode']==1 and decoded['blocks']==blocks and raw==(case/'prepared/grant.bin').read_bytes()
        assert identity==launch.sha(api.canonical(manifest))
    # Read-only capture needs its native card/range grant, without process files.
    raw=api.grant_encode(dict(grant,mode=0))
    source=out/'read-grant.bin';source.write_bytes(raw)
    capture=launch.prepare_capture(source,None,None,None,out/'capture')
    with api.ProtectedDirectory(capture) as store:
        actual,decoded,_=launch.storage_inputs(store)
        assert actual==raw and decoded['mode']==0 and decoded['first']==128
    with api.ProtectedDirectory(capture,create=True) as nested:
        assert nested.path.parent==capture and nested.path!=capture
        assert (capture/'grant.bin').read_bytes()==raw
        assert (nested.path.stat().st_mode & 0o777)==0o700
    link=out/'capture-link';link.symlink_to(capture,target_is_directory=True)
    try:api.ProtectedDirectory(link,create=True)
    except ValueError as error:assert 'symlink' in str(error)
    else:raise AssertionError('Symlink output directory admitted')
    source.write_bytes(api.grant_encode(grant))
    try:launch.prepare_capture(source,None,None,None,out/'write-capture')
    except ValueError as error:assert 'only a read-only grant' in str(error)
    else:raise AssertionError('Write grant admitted by read-only capture')
    print('PASS storage launch: complete backup, commitment tables, direct read capture and output preservation')


if __name__=='__main__':main()
