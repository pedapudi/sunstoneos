#!/usr/bin/env python3
"""Validate frozen eMMC packaging, authority refusals and actual producer capture."""
import argparse,copy,hashlib,importlib.util,json,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def run(bundle,native,out):
    out.mkdir(parents=True,exist_ok=False)
    host=module('emmc_host_capture',ROOT/'apps/emmc-read/capture.py');prep=host.shared.preparation
    frozen=host.freeze_bundle(bundle);raw=frozen['_frozen']['session.bin'];template=frozen['_frozen']['session-template.bin']
    admission=json.loads(frozen['_frozen']['admission.json']);ih=frozen['image_sha256']
    cache={p:frozen['_frozen'][v['name']] for p,v in frozen['entry_evidence'].items()}
    refused=[]
    for name,key,changes in [('population','topology',{'populated_emmc_physically_identified':False}),
       ('wrong-route','topology',{'selected_controller':'sdc2' if struct.unpack_from('<I',raw,892)[0] else 'sdc3'}),
       ('alternate-clients','topology',{'sdc2_sdc3_nand_spif_clients_excluded':False}),
       ('domain-voltage','supplies',{'pc_domain_manual_1800':False}),
       ('cold-history','initial',{'cold_card_protocol':False}),
       ('irq-ownership','initial',{'irq195_196_owned_disabled_drained':False})]:
        record=copy.deepcopy(admission);modified=dict(cache);session=bytearray(template)
        fact=json.loads(cache[record[key]['path']]);fact.update(changes)
        body=json.dumps(fact,sort_keys=True).encode();modified[record[key]['path']]=body
        digest=hashlib.sha256(body).digest();record[key]['sha256']=digest.hex()
        offset={'initial':632,'supplies':696,'topology':728}[key];session[offset:offset+32]=digest
        th=hashlib.sha256(session).hexdigest();record['session_template_sha256']=th
        try:prep.validate_admission(record,ih,th,session,modified,True)
        except ValueError:refused.append(name)
        else:raise AssertionError(name)
    frames=[]
    for name in ('idle-frame.bin','pending-frame.bin','native-frame.bin','pending-last-frame.bin','last-frame.bin'):
        frame=bytearray((native/name).read_bytes());struct.pack_into('<2Q',frame,16,*struct.unpack_from('<2Q',raw,24));frames.append(bytes(frame))
    # Only the producer's freshly randomized lifetime identity changes.
    packets=[b'\0',host.shared.decoder.read_request(19,2,1),b'\0',host.shared.decoder.read_request(33554424,8,2),b'\0']
    calls=[];record={}
    def exchange(packet,timeout):
        i=len(calls);assert packet==packets[i];calls.append(packet);return frames[i]
    result=host.capture(exchange,raw,out,record,lambda:None,time.monotonic()+10,ranges=((19,2),(33554424,8)))
    assert result['status']==1 and len(calls)==5 and record['storage_read_application_reads']==2
    # Reused SD decoding cannot accept an eMMC session.
    sd=module('emmc_host_sd_decode',ROOT/'apps/storage-read/observe.py')
    try:sd.decode(frames[-1],raw)
    except ValueError:refused.append('sd-session-identity')
    else:raise AssertionError('SD decoder accepted eMMC session')
    def reference(path):return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    result_path=out/'result.json'
    result_path.write_text(json.dumps(dict(passed=True,physical_access=False,refused=refused,image_sha256=ih,
      launcher_sha256=frozen['files']['launch.bin'],actual_native_frames=5,host_ranges=2,native_client_reads=2,
      scope='Actual admitted ELF manifest, complete fixture/bundle freeze and ARM launcher compilation. Host consumes actual native serializer frames with only epoch/generation rebound. All authority is explicitly modeled; no physical operation.'),indent=2)+'\n')
    software=dict(schema='a733-emmc-read-supervised-software-v2',physical_access=False,
      session_binding=True,read_only_trace=True,revocation_retention=True,host_capture=True,
      image=reference(bundle/'image.elf'),launcher=reference(bundle/'launch.bin'),
      sources={name:reference(path) for name,path in host.shared.software_sources().items()},
      evidence=[reference(native/'result.json'),reference(result_path)])
    (out/'software.json').write_text(json.dumps(software,indent=2)+'\n')
    host.shared.validate_software(software,frozen)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','native','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.bundle,a.native,a.output)
