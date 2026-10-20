#!/usr/bin/env python3
"""Produce deterministic ISP0 input and typed MODEL-NOT-PHYSICAL session artifacts.

Requires the actual native ELF. Copies the pinned shared workload-2 provider
profile with an explicit USB reference mode, then calls the production ISP0 session and upload producers. It emits
an unarmed ordinary contract; native instruction checks must arm that exact
contract using the actual ELF layout. It computes no reference output pixels.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1]


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result


def bayer_pattern():
    """BGGR phase biases over a deterministic ramp spanning samples 0 through 4095."""
    output=bytearray(640*480*2)
    for y in range(480):
        for x in range(640):
            # BGGR positions: B,G on even rows; G,R on odd rows.
            bias=((0,257),(257,1023))[y&1][x&1]
            struct.pack_into('<H',output,(y*640+x)*2,(x+17*y+bias)&4095)
    return bytes(output)


def produce(image,output,usb_reference_mode=1):
    prep=module('isp0_model_preparation',ROOT/'apps/isp0/prepare.py')
    producer=module('isp0_model_producer',ROOT/'apps/isp0/producer.py')
    providers=module('isp0_model_providers',ROOT/'checks/a733-provider-fixture.py')
    image=Path(image).resolve();out=Path(output).resolve()
    image_bytes=image.read_bytes();image_hash=hashlib.sha256(image_bytes).hexdigest()
    segments,address,identity=prep.image_parts(image)
    accepted=providers.source_bytes()
    board=providers.nominal_provider_session(accepted[:592],usb_reference_mode=usb_reference_mode)
    # These are software fixture identities, without target or physical authority.
    epoch,generation,lease=101,102,103
    struct.pack_into('<3Q',board,24,epoch,generation,lease);board[48:80]=identity
    target=dict(identity='isp0-software-only-model',port='no-physical-port',
        image_sha256=image_hash,physical_authority=False,modeled=True)
    data={}
    def record(name,value):
        encoded=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode()
        data[name]=encoded;return encoded
    note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
    for offset in (80,216,248,280):board[offset:offset+32]=hashlib.sha256(note).digest()
    conditions={
        'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained',
            'isp0_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent',
            'remote_secure_writers_excluded','isp0_vipp_dma_irqs_owned_masked','iommu_irqs_owned_masked'],
        'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled',
            'iommu_clocks_stable','isp0_master1_read_master2_write_mapping'],
        'electrical':['dcdc2_shared_800mv','isp0_operating_point_source_reviewed','stable_isp0_parent'],
        'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}
    facts={key:record(key+'.json',dict(**target,schema='a733-isp0-'+key+'-v1',
        reviewed=True,**{name:True for name in fields})) for key,fields in conditions.items()}
    pixels=bayer_pattern();prep.input_check(pixels)
    owner,authority,client=201,202,3
    raw=producer.extend(bytes(board),pixels,owner=owner,authority=authority,client=client,**facts)
    providers.validate_nominal_providers(raw,usb_reference_mode=usb_reference_mode)
    prep.embedded_reference(image_bytes,raw)
    request,contract=prep.request_bytes(address,identity,raw,epoch,generation,lease)
    data.update({'session.bin':raw,'input.bggr12':pixels,'upload.bin':prep.upload_bytes(pixels,raw),
        'request.bin':request,'contract-page.bin':contract,'contract.bin':contract[:224]})
    baseline=dict(schema='a733-isp0-model-fixture-v1',**target,image=str(image),
        build_id=identity.hex(),session_sha256=hashlib.sha256(raw).hexdigest(),
        epoch=epoch,generation=generation,lease=lease,owner=owner,authority=authority,client=client,
        provider_source=str(providers.SOURCE),provider_source_sha256=providers.SOURCE_SHA256,
        provider_helper_sha256=hashlib.sha256(Path(providers.__file__).read_bytes()).hexdigest(),
        producer_sha256=hashlib.sha256(Path(producer.__file__).read_bytes()).hexdigest(),
        pattern='sample=(x+17*y+BGGR-phase-bias)&4095; biases B0,G257,R1023',
        width=640,height=480,input_stride=1280,output_stride=640,
        input_bytes=len(pixels),input_sha256=hashlib.sha256(pixels).hexdigest(),
        sample_min=min(v[0] for v in struct.iter_unpack('<H',pixels)),
        sample_max=max(v[0] for v in struct.iter_unpack('<H',pixels)),
        fabric=list(struct.unpack_from('<3I',raw,488)),rates=list(struct.unpack_from('<3I',raw,500)),
        supplies=list(struct.unpack_from('<4I',raw,512)),usb_reference_mode=usb_reference_mode,usb_profile=('cold-reference' if usb_reference_mode==1 else 'rom-takeover'),pmic_pins=list(raw[564:570]),
        contract_armed=False,numerical_qualified=False,
        files={name:hashlib.sha256(value).hexdigest() for name,value in data.items()})
    out.mkdir(parents=True,exist_ok=False)
    for name,value in data.items():(out/name).write_bytes(value)
    (out/'fixture.json').write_text(json.dumps(baseline,indent=2,sort_keys=True)+'\n')
    return baseline


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--usb-reference-mode',type=int,choices=(1,2),default=1,
        help='1: modeled cold reference for ordinary SMP image; 2: requires a capable image')
    args=parser.parse_args();print(json.dumps(produce(args.image,args.output,args.usb_reference_mode),indent=2))


if __name__=='__main__':main()
