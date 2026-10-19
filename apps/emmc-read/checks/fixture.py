#!/usr/bin/env python3
"""Produce modeled eMMC authority using actual board and PMIC producers."""
import hashlib,importlib.util,json,re,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def produce(image,out,host):
    out.mkdir(parents=True,exist_ok=False)
    prep=module('emmc_fixture_prepare',ROOT/'apps/emmc-read/prepare.py')
    ih=hashlib.sha256(image.read_bytes()).hexdigest();_,_,identity=prep.base.image_parts(image)
    baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
    raw=bytearray(baseline.read_bytes());assert len(raw)==592
    raw[48:80]=identity;raw.extend(bytes(320))
    struct.pack_into('<3I',raw,488,0,1<<31,1<<31)
    struct.pack_into('<5Q',raw,592,prep.SESSION_MAGIC,2,912,23,91)
    for i,pin in enumerate(prep.PINS):raw[828+3*i:831+3*i]=bytes((3+host if pin in (5,6,10) else 0,1,0))
    struct.pack_into('<QQIIQII',raw,872,10000000,5000000,0x00300000,host,7,2,0)
    pmic_source=ROOT/'checks/a733-pmic.c'
    match=re.search(r'const struct a733_pmic_config config=\{[^{}]*\{\{([^}]+)\},\{([^}]+)\}\}\};',pmic_source.read_text());assert match
    raw[564:570]=bytes(int(v) for group in match.groups() for v in group.split(','))
    struct.pack_into('<I',raw,532,2)
    target=dict(identity='emmc-software-only-model',port='no-physical-port',image_sha256=ih)
    def record(name,value):
        data=json.dumps(value,sort_keys=True).encode();path=(out/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    evidence=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical_access=False,purpose='Software fixture only; never physical admission'))
    for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
    facts={**prep.shared.REQUIRED_FACTS,'initial':['cold_selected_controller','cold_card_protocol','all_sdc2_sdc3_clients_excluded','remote_firmware_excluded','irq195_196_owned_disabled_drained','no_dma_queue_encryption','no_clock_reset_writers','no_failed_native_lifetime']}
    refs={}
    for i,(key,required) in enumerate(facts.items()):
        refs[key]=record(key+'.json',dict(**target,schema='a733-emmc-read-'+key+'-v2',reviewed=True,physical_access=False,selected_controller='sdc2' if host==0 else 'sdc3',**{k:True for k in required}))
        raw[632+32*i:664+32*i]=bytes.fromhex(refs[key]['sha256'])
    template=out/'session-template.bin';template.write_bytes(raw);prep.session_check(raw);prep.embedded_reference(image.read_bytes(),raw)
    usb=record('usb-rom-takeover.json',dict(**target,schema='a733-usb0-rom-takeover-v1',reviewed=True,physical_access=False,**{k:True for k in ('all_usb0_clients_excluded','usb1_clients_excluded','external_dma_and_remote_writers_excluded','rom_usb0_pio_no_undrained_dma','authorize_one_disconnect_and_device_reset','preserve_usb1_and_shared_reference','whole_board_power_removal_on_uncertainty')}))
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:usb if k=='usb-clock-power-reset-ownership' else evidence for k in conditions}))
    ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
    admission=dict(**target,schema=prep.shared.ENTRY_SCHEMA,reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
    path=out/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n');prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
    (out/'fixture-inputs.json').write_text(json.dumps(dict(baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),pmic_fixture=str(pmic_source),pmic_fixture_sha256=hashlib.sha256(pmic_source.read_bytes()).hexdigest(),image_sha256=ih,physical_access=False),indent=2)+'\n')
    return template,path
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--host',type=int,choices=(0,1),required=True)
    a=p.parse_args();produce(a.image,a.output,a.host)
