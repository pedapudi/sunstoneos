#!/usr/bin/env python3
"""Suspend actual FAT32 operations and check the SD owner's independent proof."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('owner_check', ROOT/'checks/os-quiesce-storage.py')
owner_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(owner_check)
CHECK_INPUTS = owner_check.CHECK_INPUTS + ['drivers/storage/fat32_rw.c', 'include/fat32_rw.h',
                                         'checks/os-quiesce-storage-write.c']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    owner = (ROOT/'apps/os/sd_owner.c').read_text()
    library = (ROOT/'drivers/storage/fat32_rw.c').read_text()
    tracker = owner[owner.index('/* Owner-only fields;'):owner.index('static enum os_quiesce_result storage_quiesce_step')]
    header = owner_check.PREAMBLE + owner_check.OWNER_SETUP + tracker + owner_check.adapter(owner) + '\n' + owner_check.function(owner, 'owner_quiesce_step')
    serve = owner_check.function(owner, 'serve')
    assert serve.index('retirement_write(') < serve.index('hw->write_piece(')
    assert 'retirement_written(ok)' in serve
    assert '.transaction=writer_transaction,.write_classified=writer_classified' in owner
    image = args.output/'card.img'
    if not image.exists():
        with image.open('wb') as stream: stream.truncate(73728*512)
        subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '1', str(image)], check=True, capture_output=True)
    def run(name, model, source, baseline=False, card=image):
        out = args.output/name; out.mkdir(exist_ok=True)
        (out/'storage-owner-model.h').write_text(model)
        (out/'fat32_rw.c').write_text(source)
        command = ['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
                   '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(ROOT/'include'),
                   '-I'+str(ROOT/'apps/os'),str(ROOT/'checks/os-quiesce-storage-write.c'),str(out/'fat32_rw.c'),
                   str(ROOT/'apps/os/quiesce.c'),'-o',str(out/'check')]
        built = subprocess.run(command, capture_output=True, text=True)
        (out/'build.log').write_text(built.stdout+built.stderr)
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(out/'check'),str(card)]+(['baseline'] if baseline else []), capture_output=True, text=True)
        (out/'result.log').write_text(result.stdout+result.stderr)
        return result
    result = run('current',header,library)
    assert result.returncode == 0,result.stdout+result.stderr
    print(result.stdout.strip())
    before = run('baseline',header,library,baseline=True)
    assert before.returncode == 0,before.stdout+before.stderr
    def counts(output):
        return [int(line.split('write requests ')[1].split(',')[0]) for line in output.splitlines() if line.startswith('operation ')]
    assert counts(before.stdout)==counts(result.stdout), 'retirement changes native sector-request count'
    assert 'memcpy' not in owner_check.function(owner,'writer_classified')
    print('PASS before/after: same sector-request counts, two control requests per outer metadata operation, no added data-service request or owner payload copy')
    variants = {
        'missing-metadata-publication':(header.replace('transaction.dirty = 1; transaction.phase = phase;', ''),library),
        'missing-release-publication':(header.replace('retirement_publish();                 /* precedes the first destructive command */',''),library),
        'failed-commit-certified':(header.replace('event == FWR_TX_COMMIT && !transaction.failed','event == FWR_TX_COMMIT').replace('transaction.consistent = 1; transaction.dirty = 0;', 'transaction.consistent = 1; transaction.dirty = transaction.failed = 0;'),library),
        'nested-commit':(header,library.replace('if (--v->transaction_depth) return result;','--v->transaction_depth;')),
        'old-unconditional-fault':(header.replace('if (!(state >> 32) || !phase) return 0;','if (!(state >> 32)) return 0;'),library),
    }
    for name,(model,source) in variants.items():
        assert run(name,model,source).returncode != 0,name
    one = args.output/'one-fat.img'
    if not one.exists():
        with one.open('wb') as stream: stream.truncate(73728*512)
        subprocess.run(['/usr/sbin/mkfs.fat','-F','32','-s','1','-f','1',str(one)],check=True,capture_output=True)
    one_result=run('one-fat',header,library,card=one)
    assert one_result.returncode==0,one_result.stdout+one_result.stderr
    print('One FAT: '+one_result.stdout.strip().splitlines()[-1])
    wrong=library.replace('v->fats == 2 && sector >= v->fat2','sector >= v->fat2')
    assert run('one-fat-wrong-phase',header,wrong,card=one).returncode!=0
    print(f'PASS {len(variants)+1} storage retirement mutations; card effects are modeled')

if __name__ == '__main__': main()
