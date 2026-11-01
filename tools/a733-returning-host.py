#!/usr/bin/env python3
"""Compatibility utilities for historical returning hosts; tools/board.py owns launches."""
import importlib.util,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
crumbs=module('returning_crumbs',ROOT/'tools/a733-crumbs.py')

_directories=module('returning_evidence',ROOT/'tools/host_evidence.py')
output_directory=_directories.evidence_directory
artifact_directory=_directories.new_directory

class WatchdogReset(RuntimeError):
 """The armed WDT0 reset the board to a new ROM instance; DRAM is lost, the run failed."""

def retain_failure(base,record,save,quarantine,error,hold=None):
 hold=base.hold_ownership if hold is None else hold
 if getattr(base,'clean_dram_refusal',lambda r,e:False)(record,error):
  base.stop_after_failure(record,save,quarantine,error,hold=hold)
  return
 retained=False
 def hold_once():
  nonlocal retained
  if not retained:
   retained=True
   hold()
 if record.get('dirty') or record.get('dram_probe_dirty') or record.get('transport_failed') or record.get('launch_attempted'):
  try:base.stop_after_failure(record,save,quarantine,error,hold=hold_once)
  finally:
   if not record.get('launch_attempted'):hold_once()

def run(args, **callbacks):
 """Compatibility launch entry; the board runner owns admission and transport."""
 sys.path.insert(0,str(ROOT/'tools'))
 import board
 profile=callbacks.get('profile','')
 kind='npu' if 'npu' in profile.lower() else 'gpu-return' if 'gpu' in profile.lower() else 'cpu-return'
 raise SystemExit(board.legacy_run(args,kind))
