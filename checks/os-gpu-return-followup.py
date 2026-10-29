#!/usr/bin/env python3
"""GPU opt-in and returned-proof branches with a real frozen linked bundle.

No USB module, queue database or transport is opened. The bundle validator and
native-produced CPU/GPU records are real. The enclosing retained receipt is a
labeled model assembled from the accepted CPU host fixture. This exercises the
new pretransport branches; CPU compound transport/helper cases are reused.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from harness import refused, save
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_compound', ROOT/'apps/os/cpu-return-followup.py')
f = importlib.util.module_from_spec(spec); spec.loader.exec_module(f)
E = Path.home()/'.cache/sunstoneos/evidence/sol-gpu-return-1028'
C = Path.home()/'.cache/sunstoneos/evidence/sol-cpu-normal-return-1028'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    bundle = E/'preparation-delivery/MODEL-NOT-DEPLOYABLE-clock-top'
    p = f.cpu.validate_bundle(bundle)
    assert p['image_sha256'] == '6d28a8340c08ba4a3d07eb46a6cd578cb8b8a72f9102c43052e31690f8c89c66'
    config = json.loads((bundle/'board/image-input-manifest.json').read_text())['gpu_profile']
    report = (E/'terminal-delivery/return-report.bin').read_bytes()
    output = (E/'capture-producer-delivery/opened-output.bin').read_bytes()
    gpu_raw = (E/'capture-producer-delivery/opened-gpu-report.bin').read_bytes()
    gpu = f.cpu.os_image.gpu_return_rules.decode(gpu_raw, p['epoch'], p['generation'], f.require)
    text = f.cpu.decode_output(output, p['epoch'], p['generation'])
    cases = []
    def ref(path): return {'path':str(path.resolve()), 'sha256':f.digest(path)}
    with tempfile.TemporaryDirectory(prefix='MODEL-GPU-followup-') as temporary:
        root = Path(temporary)
        def fresh():
            (root/'return-report.bin').write_bytes(report)
            (root/'recorded-output.bin').write_bytes(output)
            (root/'transcript.txt').write_text(text)
            (root/'gpu-retirement.bin').write_bytes(gpu_raw)
            r = copy.deepcopy(json.loads((C/'compatibility-host/MODEL-return-receipt.json').read_text())['model_record'])
            r.update(port=p['_review']['port'], initial_node='/dev/bus/usb/099/001',
                initial_identity=p['_review']['identity'], returned_node='/dev/bus/usb/099/002',
                returned_identity=[0,0,2,0,0], image_sha256=p['image_sha256'],
                return_review_sha256=f.digest(bundle/'return-review.json'),
                host_source_sha256=f.digest(Path(f.cpu.__file__)), automatic_access=True,
                tool_sha256=f.base.FEL_HASH, report_sha256=f.digest(root/'return-report.bin'),
                output_sha256=f.digest(root/'recorded-output.bin'),
                gpu_report_sha256=f.digest(root/'gpu-retirement.bin'),
                result={**f.cpu.decode_report(report,p['epoch']), 'gpu':copy.deepcopy(gpu)})
            a = {'cpu_bundle':str(bundle), 'cpu_return_review':ref(bundle/'return-review.json'),
                 'return_session':str(root/'session.json'), 'claim':r['root_owned_claim'],
                 'port':r['port'], 'initial_node':r['initial_node'], 'initial_identity':r['initial_identity'],
                 'helper':{'image':ref(C/'rails-800/build/probe.elf'),
                           'binary':ref(C/'rails-800/probe.bin'), 'manifest':ref(C/'rails-800/input-identities.json')},
                 'gpu_return':{'configuration':copy.deepcopy(config),
                               'retirement_review_sha256':f.digest(bundle/'return-review.json'),
                               'report':str(root/'gpu-retirement.bin')}}
            return a,r
        variants = ('success','adjacent-chunks','missing-opt-in','changed-geometry','changed-review',
                    'wrong-capture-path','missing-record','changed-record','wrong-epoch','wrong-generation',
                    'missing-fw-off','no-completed-jobs','wrong-supply','summary-mismatch','missing-backups',
                    'uncached-hole','writeback-hole','oversize-backup','malformed-backup')
        for variant in variants:
            a,r = fresh()
            if variant == 'adjacent-chunks':
                lo,hi = p['qualification']['gpu']['write_back_pool']; mid=(lo+hi)//2
                r['dram_backups']=[{'address':lo,'bytes':mid-lo},{'address':mid,'bytes':hi-mid},
                    {'address':p['qualification']['gpu']['uncached_pool'][0],
                     'bytes':p['qualification']['gpu']['uncached_pool'][1]-p['qualification']['gpu']['uncached_pool'][0]}]
            if variant == 'missing-opt-in':a.pop('gpu_return')
            if variant == 'changed-geometry':a['gpu_return']['configuration']['return']['uncached_pool'][1] += 4096
            if variant == 'changed-review':a['gpu_return']['retirement_review_sha256']='0'*64
            if variant == 'wrong-capture-path':a['gpu_return']['report']=str(root/'other.bin')
            if variant == 'missing-record':(root/'gpu-retirement.bin').unlink()
            if variant == 'changed-record':(root/'gpu-retirement.bin').write_bytes(bytes(64))
            changes={'wrong-epoch':(3,p['epoch']+1),'wrong-generation':(4,p['generation']+1),
                     'missing-fw-off':(6,62),'no-completed-jobs':(5,0),'wrong-supply':(7,900000)}
            if variant in changes:
                raw=bytearray(gpu_raw);index,value=changes[variant];struct.pack_into('<Q',raw,index*8,value)
                (root/'gpu-retirement.bin').write_bytes(raw);r['gpu_report_sha256']=f.digest(root/'gpu-retirement.bin')
            if variant == 'summary-mismatch':r['result']['gpu']['worker_parked']=False
            if variant == 'missing-backups':r.pop('dram_backups')
            if variant in ('uncached-hole','writeback-hole'):
                lo,hi=p['qualification']['gpu']['uncached_pool' if variant=='uncached-hole' else 'write_back_pool']
                r['dram_backups']=[row for row in r['dram_backups'] if not row['address']<=lo<row['address']+row['bytes']]
            if variant == 'oversize-backup':r['dram_backups']=[{'address':0x41000000,'bytes':0x1200000}]
            if variant == 'malformed-backup':r['dram_backups']=[{'address':'invalid','bytes':4096}]
            (root/'session.json').write_text(json.dumps(r))
            if variant in ('success','adjacent-chunks'):
                actual,transcript=f.returned(a,p);assert actual['result']['gpu']==gpu and transcript==text
            else:
                reason = ('CPU-only profile' if variant=='missing-opt-in' else
                    'Exact GPU continuation' if variant in ('changed-geometry','changed-review','wrong-capture-path') else
                    'Copied GPU retirement' if variant in ('missing-record','changed-record') else
                    'GPU retirement' if variant in changes else
                    'receipt summary' if variant=='summary-mismatch' else
                    'coverage is absent' if variant=='missing-backups' else
                    'coverage is incomplete' if variant in ('uncached-hole','writeback-hole') else
                    'exceeds the original' if variant=='oversize-backup' else 'Invalid GPU backup')
                refused(lambda:f.returned(a,p),reason)
            cases.append({'name':variant,'passed':True,'transport_calls':0})
        a,_=fresh(); cpu=copy.deepcopy(p);cpu['qualification'].pop('gpu')
        refused(lambda:f.gpu_authority(a,cpu),'GPU authority supplied for a CPU-only')
        a.pop('gpu_return');assert f.gpu_authority(a,cpu) is None
        cases.append({'name':'CPU-only-default-and-extra-proof-refusal','passed':True,'transport_calls':0})
    save(args.out/'results.json', {'physical_access':False,'scope':__doc__, 'image_sha256':p['image_sha256'],
        'bundle_clock_authority':'nominal model; compound under-test admission is unchanged and not bypassed',
        'cases':cases,'native_producer_records':{str(E/n):f.digest(E/n) for n in
            ('terminal-delivery/return-report.bin','capture-producer-delivery/opened-gpu-report.bin','capture-producer-delivery/opened-output.bin')}})
    print('PASS',len(cases),'GPU compound pretransport branches')

if __name__ == '__main__': main()
