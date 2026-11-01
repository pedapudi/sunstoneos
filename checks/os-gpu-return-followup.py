#!/usr/bin/env python3
"""Validate generic GPU retirement reports captured by the linked native fixtures.

The OS decoder and retirement-review predicates execute against reports emitted
by os-gpu-return.py and os-gpu-untouched.py. Mutations cover report identity,
shutdown conditions, untouched-return opt-in and review evidence. Transport,
bundle authority, retained backups and compound host followup belong to the
application launcher and are outside this parser check. No hardware is accessed.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from harness import refused, save

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_return_rules',ROOT/'apps/os/gpu-return.py')
rules=importlib.util.module_from_spec(spec);spec.loader.exec_module(rules)


def require(ok,reason):
    if not ok:raise ValueError(reason)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mutated(raw,index,value):
    result=bytearray(raw);struct.pack_into('<Q',result,index*8,value);return bytes(result)


def captures(directory,pattern):
    summary=json.loads((directory/'results.json').read_text())
    assert summary['physical_access'] is False and summary['cases']
    assert all(case['passed'] for case in summary['cases'])
    paths=sorted(directory.glob(pattern));assert paths,'Native fixture reports are absent: '+str(directory)
    return summary,paths


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output','--out',dest='output',type=Path,required=True)
    parser.add_argument('--return-captures',type=Path,required=True,
                        help='Successful os-gpu-return.py capture directory')
    parser.add_argument('--untouched-captures',type=Path,required=True,
                        help='Successful os-gpu-untouched.py capture directory')
    parser.add_argument('--case',action='append',help='Run selected cases by exact name')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    return_summary,returned=captures(args.return_captures,'*-gpu-report.bin')
    untouched_summary,untouched=captures(args.untouched_captures,'*-report.bin')
    rows=[];available=set()

    def run(name,action):
        available.add(name)
        if args.case and name not in args.case:return
        action();rows.append(dict(name=name,passed=True,transport_calls=0))

    def decode(raw,epoch,generation,allow=False):
        return rules.decode(raw,epoch,generation,require,allow_untouched=allow)

    epoch,generation=struct.unpack_from('<2Q',returned[0].read_bytes(),24)
    for path in returned:
        raw=path.read_bytes()
        def nominal(raw=raw):
            result=decode(raw,epoch,generation)
            assert result=={'jobs':struct.unpack_from('<Q',raw,40)[0],'gpu_off':True,'worker_parked':True,
                            'pmic_sample_retired':True,'dcdc4_uv':960000}
            assert decode(raw,epoch,generation,True)==result
        run('returned-'+path.stem,nominal)
    raw=returned[0].read_bytes()
    for name,index,value in (
        ('wrong-magic',0,0),('wrong-version',1,3),('wrong-size',2,63),
        ('wrong-epoch',3,epoch+1),('wrong-generation',4,generation+1),
        ('maximum-sequence',5,0xffffffff),
        ('oversize-sequence',5,0x100000000),('wrong-supply',7,900000)):
        run(name,lambda index=index,value=value:refused(
            lambda:decode(mutated(raw,index,value),epoch,generation),'GPU retirement'))
    for bit in range(6):
        run('missing-shutdown-condition-'+str(bit),lambda bit=bit:refused(
            lambda:decode(mutated(raw,6,63^(1<<bit)),epoch,generation),'GPU retirement'))
    run('unknown-shutdown-condition',lambda:refused(
        lambda:decode(mutated(raw,6,127),epoch,generation),'GPU retirement'))
    for name,data in (('short-record',raw[:-1]),('extra-record-byte',raw+b'\0')):
        run(name,lambda data=data:refused(lambda:decode(data,epoch,generation),'Incomplete GPU retirement'))

    untouched_epoch,untouched_generation=struct.unpack_from('<2Q',untouched[0].read_bytes(),24)
    for path in untouched:
        data=path.read_bytes();stage=struct.unpack_from('<Q',data,56)[0]
        def nominal(data=data,stage=stage):
            refused(lambda:decode(data,untouched_epoch,untouched_generation),'not admitted')
            result=decode(data,untouched_epoch,untouched_generation,True)
            expected={'jobs':0,'gpu_untouched':True,'qualification_passed':False}
            expected.update({'device_never_opened':True} if stage==0 else
                            {'constructor_refusal':stage,'constructor_stage':rules.CONSTRUCTOR_STAGES[stage]})
            assert result==expected
            assert not any(result.get(key,False) for key in ('gpu_off','worker_parked','pmic_sample_retired'))
        run('untouched-'+path.stem,nominal)
    data=untouched[0].read_bytes()
    for name,index,value in (
        ('untouched-wrong-magic',0,0),('untouched-wrong-size',2,63),
        ('untouched-wrong-epoch',3,untouched_epoch+1),
        ('untouched-wrong-generation',4,untouched_generation+1),
        ('untouched-has-job',5,1),('untouched-has-shutdown-proof',6,63),
        ('untouched-stage-outside',7,15)):
        run(name,lambda index=index,value=value:refused(
            lambda:decode(mutated(data,index,value),untouched_epoch,untouched_generation,True),
            'GPU untouched return'))
    # An untouched receipt cannot become a normal shutdown receipt by changing
    # its version: it has no shutdown conditions or supply observation.
    run('untouched-cannot-claim-shutdown',lambda:refused(
        lambda:decode(mutated(data,1,1),untouched_epoch,untouched_generation,True),'GPU retirement'))

    config={'profile':'modeled review input','return':{'write_back_pool':[0x41200000,0x41400000],
                                                     'uncached_pool':[0x41c00000,0x41e00000]}}
    conditions={name:{'evidence':name} for name in sorted(rules.CONDITIONS)}
    baseline={'gpu_retirement':{'configuration':copy.deepcopy(config),'conditions':conditions}}
    observed=[]
    def evidence(value):
        require(isinstance(value,dict) and value.get('evidence') in rules.CONDITIONS,
                'Review evidence is absent.')
        observed.append(value['evidence'])
    def nominal_review():
        observed.clear();rules.validate(baseline,config,evidence,require)
        assert observed==sorted(rules.CONDITIONS)
    run('exact-retirement-review',nominal_review)
    for name in ('missing-review','wrong-configuration','missing-condition','extra-condition','missing-evidence'):
        def invalid_review(name=name):
            review=copy.deepcopy(baseline)
            if name=='missing-review':review.clear()
            elif name=='wrong-configuration':review['gpu_retirement']['configuration']['return']['uncached_pool'][1]+=4096
            elif name=='missing-condition':review['gpu_retirement']['conditions'].pop(next(iter(conditions)))
            elif name=='extra-condition':review['gpu_retirement']['conditions']['unsupported']={}
            else:review['gpu_retirement']['conditions'][next(iter(conditions))]={}
            refused(lambda:rules.validate(review,config,evidence,require),
                    'Review evidence' if name=='missing-evidence' else 'Exact GPU final-retirement review')
        run(name,invalid_review)
    if args.case:assert set(args.case)<=available,'Unknown case: '+str(set(args.case)-available)
    inputs={str(path.resolve()):digest(path) for path in [*returned,*untouched]}
    save(args.output/'results.json',dict(physical_access=False,scope=__doc__,cases=rows,
        decoder_sha256=digest(ROOT/'apps/os/gpu-return.py'),native_producer_records=inputs,
        producer_images={'return':return_summary['image_sha256'],'untouched':untouched_summary['image_sha256']},
        remaining_external_coverage='Application launcher compound followup: bundle and opt-in authority, copied-record paths/hashes, retained backup coverage and transport.'))
    print('PASS',len(rows),'GPU retirement decoder and review cases; no transport')


if __name__=='__main__':main()
