#!/usr/bin/env python3
"""Create an explicit RTC initialization template for subsequent reviewed admission."""
import argparse
import datetime
import hashlib
from pathlib import Path
import prepare

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('adoption-template','clock-evidence','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--service',action='store_true',help='Admit the two fixed native clients for reusable task-side access')
    parser.add_argument('--runtime',action='store_true',help='Set calendar once in owner task after providers start')
    parser.add_argument('--initialize-source',action='store_true')
    parser.add_argument('--calendar',help='Explicit calendar value YYYY-MM-DDTHH:MM:SS; no current-time accuracy claim')
    args=parser.parse_args();args.runtime=args.runtime or args.service;flags=int(args.initialize_source);day=hms=0
    if args.calendar:
        target=datetime.datetime.strptime(args.calendar,'%Y-%m-%dT%H:%M:%S')
        day,hms=prepare.calendar_target(target.year,target.month,target.day,target.hour,target.minute,target.second);flags|=4 if args.runtime else 2
    if args.runtime and not args.calendar:parser.error('--runtime needs an explicit --calendar')
    raw=prepare.setting_session(args.adoption_template.read_bytes(),flags,day,hms,hashlib.sha256(args.clock_evidence.read_bytes()).digest(),service=args.service)
    with args.output.open('xb') as output:output.write(raw)
if __name__=='__main__':main()
