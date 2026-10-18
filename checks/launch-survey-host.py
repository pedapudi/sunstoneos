#!/usr/bin/env python3
"""One production survey post-exe fault check; all transport/access is mocked."""
import argparse
from contextlib import contextmanager
import fcntl
import importlib.util
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','software','evidence'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('survey_host',ROOT/'tools/run-launch-survey.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
def ref(path):return {'path':str(path.resolve()),'sha256':host.digest(path)}
fel=a.evidence/'fixture-fel';fel.write_bytes(b'Not executable: software-only FEL fixture')
recovery=a.evidence/'recovery.json';recovery.write_text(json.dumps({'identity':[1,2,3,4,5],'port':'3-1'}))
prerequisites={'supervised-ram-recovery':ref(recovery),'launch-survey-software':ref(a.software)}
dbpath=a.evidence/'queue.db';db=sqlite3.connect(dbpath)
db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
submission={'prerequisites':list(prerequisites),'artifacts':[ref(a.build/n) for n in ('manifest.json','launch-survey.bin')]}
db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','owner',json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit();db.close()
lock=a.evidence/'private.lock';events=[];memory={};node=Path('/fixture-usb/003/072')
@contextmanager
def board_lock():
    with lock.open('a') as owned:
        fcntl.flock(owned,fcntl.LOCK_EX|fcntl.LOCK_NB);yield node
access=types.SimpleNamespace(board_lock=board_lock,require_rom=lambda n:None,identity=lambda n:[1,2,3,4,5],target=lambda n:Path('/fixture-port/3-1'),quarantine_instance=lambda *a:events.append('quarantine'))
def fake_fel(command,**kwargs):
    assert command[:3]==[str(fel),'-d','3:72'];args=command[3:];events.append(args)
    if args==['version']:return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
    if args==['exe','0x85000']:raise subprocess.TimeoutExpired(command,15)
    i=0
    while i<len(args):
        op,address=args[i],int(args[i+1],16)
        if op=='read':size=int(args[i+2]);Path(args[i+3]).write_bytes(memory.get(address,bytes([0xa5])*size)[:size]);i+=4
        else:assert op=='write' and kwargs['pass_fds'];memory[address]=Path(args[i+2]).read_bytes();i+=3
    return types.SimpleNamespace(returncode=0,stdout='',stderr='')
def hold():
    with lock.open('a') as other:
        try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:events.append('held-private-flock')
        else:raise AssertionError('Released survey flock before hold')
real_stop=host.base.stop_after_failure
def stop(record,save,quarantine,error):
    assert record['launch_attempted'] is True
    return real_stop(record,save,quarantine,error,hold=hold)
guard=types.ModuleType('usb_guard');guard.check_health=lambda:None
args=types.SimpleNamespace(build=a.build.resolve(),software=a.software.resolve(),database=dbpath,claim='fixture',owner='owner',fel=fel,evidence=a.evidence/'runtime')
with patch.object(host,'module',lambda *a:access),patch.object(host.base,'FEL_HASH',host.digest(fel)),patch.object(host.subprocess,'run',fake_fel),patch.object(host.base,'stop_after_failure',stop),patch.dict(sys.modules,{'usb_guard':guard}):
    try:host.run(args)
    except subprocess.TimeoutExpired:pass
    else:raise AssertionError('Expected single execution timeout')
record=json.loads((args.evidence/'session.json').read_text())
assert events[-3:]==[['exe','0x85000'],'quarantine','held-private-flock']
assert record['launch_attempted'] and record['transport_failed'] and not record['complete']
with lock.open('a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
host.save(a.evidence/'results.json',{'physical_access':False,'case':'post-exe transport timeout retains real fixture flock','pass':True,'events':events,'host_sha256':host.digest(Path(host.__file__))})
print('PASS one post-exe fault case; exact production helper retention predicate exercised.')
