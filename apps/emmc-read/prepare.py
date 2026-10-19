#!/usr/bin/env python3
"""Prepare a read-only eMMC lifetime; physical population and route need authority."""
import importlib.util
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
shared=module('emmc_read_shared_prepare',ROOT/'apps/storage-read/prepare.py')
require=shared.require
SESSION_MAGIC=0x4153373345524131
SESSION_BYTES=912
shared.CONTRACT_MAGIC=0x4153373345524331
shared.REQUEST_MAGIC=0x4153373345524c31
shared.SESSION_MAGIC=SESSION_MAGIC
shared.SESSION_BYTES=SESSION_BYTES
shared.SESSION_DEFINE=['-DSUNSTONE_EMMC_READ=1']
shared.PROFILES={'sunstone-a733-emmc-read':'emmc-read-single-core-supervised-v2'}
shared.SINGLE_CORE_TARGET='sunstone-a733-emmc-read'
shared.SINGLE_CORE_BYTES=SESSION_BYTES
shared.SCHEMA='a733-emmc-read-supervised-bundle-v2'
shared.AUTHORITY_PREFIX='a733-emmc-read-'
shared.ENTRY_SCHEMA='a733-emmc-read-supervised-entry-v2'
shared.REQUIRED_FACTS={**shared.REQUIRED_FACTS,
 'supplies':['dcdc1_dc1sw2_3300000_cldo5_1800000','source_qualified_settling','pc_domain_manual_1800','no_voltage_writers'],
 'topology':['populated_emmc_physically_identified','selected_controller_route_source_qualified',
 'pc0_1_5_6_8_11_13_16_complete_pin_authority','pc1_reset_observed_high_never_driven',
 'source_qualified_pulls_drives','sdc2_sdc3_nand_spif_clients_excluded','no_alternate_pad_writers']}
PINS=(0,1,5,6,8,9,10,11,13,14,15,16)
def session_check(raw):
    require(len(raw)==SESSION_BYTES,'eMMC session must be 912 bytes.')
    require(shared.board.session_check(raw[:592],usb_reference_modes=(1,2))==2,'eMMC requires USB0/provider workload 2.')
    require(struct.unpack_from('<3Q',raw,592)==(SESSION_MAGIC,2,SESSION_BYTES),'Invalid eMMC session identity.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid eMMC owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728,760)),'Missing eMMC authority.')
    require(raw[864:872]==bytes(8),'Nonzero eMMC reserved bytes.')
    identify,read,voltage,host=struct.unpack_from('<QQII',raw,872)
    require(0<identify<=10000000 and 0<read<=5000000 and voltage and not voltage&~0x00300000 and host in (0,1),'Invalid eMMC controller or bounded profile.')
    first,blocks,reserved=struct.unpack_from('<QII',raw,896)
    require(1<=blocks<=8 and not reserved and first<=0xffffffff and blocks-1<=0xffffffff-first,'Invalid eMMC application range.')
    for i,pin in enumerate(PINS):
        ef,ep,ed=raw[792+3*i:795+3*i];df,dp,dd=raw[828+3*i:831+3*i]
        require(ef in (0,15) and ep<=2 and ed<=3 and df==(3+host if pin in (5,6,10) else 0) and dp<=2 and dd<=3 and (pin in (0,5) or dp==1),'Invalid one-bit PC route.')
def initial_policy(fact):
    require(all(fact.get(k) is True for k in ('cold_selected_controller','cold_card_protocol',
      'all_sdc2_sdc3_clients_excluded','remote_firmware_excluded','irq195_196_owned_disabled_drained',
      'no_dma_queue_encryption','no_clock_reset_writers','no_failed_native_lifetime')),'Missing cold eMMC ownership authority.')
    require(fact.get('initialization_mode','cold')=='cold','eMMC requires established cold card history.')
# The common producer calls these hooks when freezing and revalidating bundles.
shared.session_check=session_check
shared.initial_policy=initial_policy
shared.inherited_mux=lambda raw:1
_shared_validate=shared.validate_admission
def _bound_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    session_check(raw)
    cache=_shared_validate(record,image_hash,template_hash,raw,cache,frozen)
    import json
    selected='sdc2' if struct.unpack_from('<I',raw,892)[0]==0 else 'sdc3'
    require(all(json.loads(cache[record[k]['path']]).get('selected_controller')==selected for k in ('initial','topology')),'eMMC authority selects another controller.')
    return cache
shared.validate_admission=_bound_admission
validate_admission=_bound_admission
prepare=shared.prepare
validate_bundle=shared.validate_bundle
embedded_reference=shared.embedded_reference
image_manifest_check=shared.image_manifest_check
build_launcher=shared.build_launcher
base=shared.base
board=shared.board
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
