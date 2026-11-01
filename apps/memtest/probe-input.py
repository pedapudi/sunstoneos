#!/usr/bin/env python3
"""Create the first CPU-only memtest input; establishes no physical authority."""
import argparse
import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('memtest_probe_input',Path(__file__).with_name('check.py'))
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--sequence',type=int,default=1)
p.add_argument('--budget',type=int,default=18)
p.add_argument('--seed',type=lambda v:int(v,0),default=0x1234567812345678)
a=p.parse_args()
if not 1<=a.budget<=18 or not 0<a.sequence<2**64:p.error('Use a positive sequence and a budget from 1 to 18 seconds.')
contract=host.make_contract(a.sequence,7,a.seed,a.budget*host.COUNTER_HZ,(0,0),[])
a.output.parent.mkdir(parents=True,exist_ok=True)
with a.output.open('xb') as f:f.write(bytes(contract))
print(a.output)
