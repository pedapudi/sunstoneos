"""Check direct HDMI inputs and native safety bounds using preserved images."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import harness

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('pattern', 'audio', 'usb', 'output', 'clang', 'lld', 'objcopy'):
        parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args()
    prep = module('direct_hdmi_audio', ROOT/'apps/hdmi-audio/prepare.py')
    modeled = module('direct_hdmi_model', ROOT/'checks/hdmi-audio-usb-preparation.py').modeled
    output = prep.base.unique_directory(args.output)
    checks = []

    pattern_image = (args.pattern/'image.elf').read_bytes()
    size = prep.base.image_data(pattern_image)[3]
    pattern = bytearray((args.pattern/'session.bin').read_bytes()[:size])
    pattern[80:112] = hashlib.sha256(encoded({})).digest()
    folder = output/'pattern'
    folder.mkdir()
    (folder/'image.elf').write_bytes(pattern_image)
    (folder/'session.bin').write_bytes(pattern)
    (folder/'inputs.json').write_bytes(encoded({}))
    prep.base.validate(folder/'image.elf', folder/'session.bin', folder/'inputs.json')
    checks.append('pattern-without-process-records')
    # Run the actual packer and compile its launcher into an existing directory.
    bundle = folder/'bundle'
    bundle.mkdir()
    (bundle/'earlier.log').write_text('preserve')
    command = [sys.executable, '-B', str(ROOT/'apps/hdmi/prepare.py'),
        '--image', str(folder/'image.elf'), '--session', str(folder/'session.bin'),
        '--admission', str(folder/'inputs.json'), '--output', str(bundle),
        '--clang', str(args.clang), '--lld', str(args.lld), '--objcopy', str(args.objcopy)]
    result = subprocess.run(command, capture_output=True, text=True)
    (folder/'build.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    assert (bundle/'earlier.log').read_text() == 'preserve'
    assert len(list(bundle.glob('*/prepared.json'))) == 1
    checks.append('pattern-actual-packer-existing-output')
    for label, parts, framebuffer in (
        ('overlap', [(0x41000000, 4096, b'a'), (0x41000000, 4096, b'b')], False),
        ('result-overwrite', [(0x42000000, 4096, b'a')], False),
        ('framebuffer-missing', [(0x41000000, 4096, b'a')], True)):
        harness.refused(lambda:prep.base.geometry(parts,framebuffer),
            {'overlap':'Overlapping', 'result-overwrite':'Initialized image', 'framebuffer-missing':'Missing complete'}[label])
        checks.append('pattern-'+label)

    for kind in ('audio', 'usb'):
        source = getattr(args, kind)
        frozen = {path.name:path.read_bytes() for path in source.iterdir() if path.is_file()}
        image = frozen['image_elf.bin']
        raw = bytearray(frozen['source-session.bin'])
        pcm = frozen['pcm.bin']
        admission = json.loads(frozen['admission.bin'])
        direct = {key:admission[key] for key in ('physical_authority','modeled','identity','port')}
        records = {'sink_edid':frozen['sink_edid.bin']}
        for key, offset in (('audio_memory',472),('audio_electrical',504),
                            ('usb_initial',672),('usb_electrical',704),('usb_supervision',736)):
            if key.startswith('usb_') and kind != 'usb':continue
            fact = json.loads(frozen[key+'.bin'])
            for field in ('schema','reviewed','image_sha256','hdmi_analog_profile_reviewed'):
                fact.pop(field,None)
            direct[key] = fact
            records[key] = encoded(fact)
            raw[offset:offset+32] = hashlib.sha256(records[key]).digest()
        security = json.loads(frozen['dma_nonsecure_access.bin'])
        for field in ('schema','reviewed','evidence'):security.pop(field,None)
        direct['dma_nonsecure_access'] = security
        records['dma_nonsecure_access'] = encoded(security)
        direct['sink_edid'] = 'sink.edid'
        admission_data = encoded(direct)
        raw[80:112] = hashlib.sha256(admission_data).digest()
        folder = output/kind
        folder.mkdir()
        for name,data in (('image.elf',image),('session.bin',raw),('input.pcm',pcm),
                          ('inputs.json',admission_data),('sink.edid',records['sink_edid'])):
            (folder/name).write_bytes(data)
        # Explicitly modeled physical decisions stay confined to this host check.
        with modeled(prep):
            prep.validate(folder/'image.elf',folder/'session.bin',folder/'inputs.json',folder/'input.pcm')
        checks.append(kind+'-inline-facts-without-process-records')
        harness.refused(lambda:prep.validate_data(image,raw,admission_data,pcm,records),'Modeled HDMI audio')
        checks.append(kind+'-production-refuses-modeled-facts')
        nominal_records = copy.deepcopy(records)
        for label in ('memory-owner','dma-access','edid','pcm','raw-clock','raw-session-image'):
            trial = bytearray(raw)
            facts = copy.deepcopy(nominal_records)
            source_pcm = pcm
            if label == 'memory-owner':
                fact = json.loads(facts['audio_memory']);fact['remote_dma_writers_excluded'] = False
                facts['audio_memory'] = encoded(fact);trial[472:504] = hashlib.sha256(facts['audio_memory']).digest()
                reason = 'typed memory authority'
            elif label == 'dma-access':
                fact = json.loads(facts['dma_nonsecure_access']);fact['descriptor_data_access_nonsecure'] = False
                facts['dma_nonsecure_access'] = encoded(fact);reason = 'access/lifetime'
            elif label == 'edid':
                facts['sink_edid'] = bytes(256);reason = 'another sink'
            elif label == 'pcm':source_pcm = b'bad';reason = 'PCM identity'
            elif label == 'raw-clock':struct.pack_into('<I',trial,300,25000000);reason = 'Unsupported HDMI TOP'
            elif label == 'raw-session-image':trial[48] ^= 1;reason = 'bind this image'
            with modeled(prep):
                harness.refused(lambda:prep.validate_data(image,trial,admission_data,source_pcm,facts),reason)
            checks.append(kind+'-'+label)
        if kind == 'usb':
            for offset,label in ((656,'owner'),(768,'reference'),(776,'supply')):
                trial = bytearray(raw)
                if offset == 656:trial[656:664] = raw[600:608]
                else:struct.pack_into('<I',trial,offset,1 if offset == 776 else 2)
                harness.refused(lambda:prep.session_check(trial,pcm),
                    'USB owner aliases' if offset == 656 else 'exact rails')
                checks.append(kind+'-'+label)
            for key,offset in (('usb_initial',672),('usb_electrical',704),('usb_supervision',736)):
                trial=bytearray(raw);facts=copy.deepcopy(records);fact=json.loads(facts[key])
                fact[prep.USB_FACTS[key[4:]][0]]=False;facts[key]=encoded(fact)
                trial[offset:offset+32]=hashlib.sha256(facts[key]).digest()
                with modeled(prep):harness.refused(lambda:prep.validate_data(image,trial,admission_data,pcm,facts),'authority is incomplete')
                checks.append(kind+'-'+key+'-facts')
        bundle = folder/'bundle'
        bundle.mkdir();(bundle/'earlier.log').write_text('preserve')
        with modeled(prep):
            prepared=prep.prepare(folder/'image.elf',folder/'session.bin',folder/'inputs.json',folder/'input.pcm',
                bundle,args.clang,args.lld,args.objcopy)
        assert prepared['image_sha256']==prep.sha(image)
        assert (bundle/'earlier.log').read_text()=='preserve'
        generated=list(bundle.glob('*/prepared.json'));assert len(generated)==1
        metadata=json.loads(generated[0].read_bytes());metadata.pop('schema')
        generated[0].write_bytes(encoded(metadata))
        # Archived compiler inputs and build logs do not authorize transfers.
        (generated[0].parent/'launch.c').write_bytes(b'archived source copy changed')
        with modeled(prep):prep.validate_bundle(generated[0].parent)
        checks.append(kind+'-actual-packer-existing-output-no-schema')
    report=dict(passed=True,checks=checks,physical_access=False,physical_qualified=False)
    (output/'result.json').write_bytes(encoded(report));print(json.dumps(report))


if __name__ == '__main__':main()
