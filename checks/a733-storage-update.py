#!/usr/bin/env python3
"""One connected storage/host integration batch; no physical access or old suites."""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


def pattern(size,seed):
    return bytes((i*131+seed*17+i//251)&255 for i in range(size))


def refused(action):
    try:action()
    except ValueError:return
    raise AssertionError('Planning or preparation admitted a mismatch')


def planned_card_update(host,fixture,base,Session,passed):
    """Plan a boot-card install from a boot-slot plan, then run each session."""
    slots=host.tool_module('boot_slots','boot-slots.py')
    work=fixture/'card-plan';work.mkdir()
    sectors=131072
    def tool(*argv):
        with (work/'boot-slots.log').open('a') as log,contextlib.redirect_stdout(log):slots.main([str(v) for v in argv])
    # Stock card: MBR partition, boot0, a nonzero backup package unit and a TOC1.
    card=bytearray(sectors*512)
    struct.pack_into('<4xB3xII',card,446,0x83,65536,65536);card[510:512]=b'\x55\xaa'
    card[16*512:496*512]=pattern(480*512,1);card[24576*512:24640*512]=pattern(64*512,2)
    card[32800*512:32834*512]=pattern(34*512,3)
    original=bytes(card);image=work/'card.img';image.write_bytes(original)
    allocation=work/'allocation.json'
    tool('allocate','--backup',image,'--slot-a','40960:8192','--slot-b','49152:8192','--output',allocation)
    boot0,package=pattern(245760,4),pattern(16384+1024,5)
    container=bytearray(20<<20);container[8192:8192+len(boot0)]=boot0;container[16793600:16793600+len(package)]=package
    (work/'synthetic.microsd-monitor-container.bin').write_bytes(container)
    (work/'synthetic.manifest.json').write_text(json.dumps({'kind':'a733-microsd-monitor-container',
        'files':{'microsd-monitor-container.bin':{'bytes':len(container),'sha256':hashlib.sha256(container).hexdigest()}},
        'card_writes':[{'name':'boot0','offset':8192,'bytes':len(boot0),'sha256':hashlib.sha256(boot0).hexdigest()},
                       {'name':'monitor-package','offset':16793600,'bytes':len(package),'sha256':hashlib.sha256(package).hexdigest()}]}))
    (work/'image').write_bytes(pattern(6000,6));(work/'asset').write_bytes(pattern(3000,7))
    tool('plan','--allocation',allocation,'--card-image',image,'--image',f'{work/"image"}@0x41000000','--entry','0x41000000',
         '--asset',f'{work/"asset"}@0x44000000','--container',work/'synthetic.manifest.json','--output',work/'slot-plan')
    slot_plan=work/'slot-plan'/'plan.json'
    record=dict(kind=1,host=0,cid=base['cid'],sectors=sectors,target=dict(port='model',sysfs='model'))
    refused(lambda:host.plan(slot_plan,allocation,image,dict(record,kind=2),work/'refused-card'))
    planned=host.plan(slot_plan,allocation,image,record,work/'update-plan')
    sessions=planned['sessions']
    assert [s['name'] for s in sessions]==['payload','header','boot0','monitor-package'] and not planned['skipped']
    assert [(s['first'],s['blocks']) for s in sessions[2:]]==[(16,480),(32800,34)]
    assert all(not slots.overlaps(s['first'],s['blocks'],*host.STOCK_BACKUP_UNIT) for s in sessions)
    unit=planned['stock_backup_unit']
    assert not unit['zero'] and unit['sha256']==hashlib.sha256(original[24576*512:32768*512]).hexdigest()
    passed.append('boot-slot plan with container becomes four single-range sessions; gaps and the stock backup unit stay unplanned')
    # A changed card, a write outside its region and a foreign session refuse.
    changed=work/'changed.img';changed.write_bytes(original[:8192*512]+b'x'+original[8192*512+1:])
    refused(lambda:host.plan(slot_plan,allocation,changed,record,work/'refused-changed'))
    forged=work/'forged-plan';shutil.copytree(work/'slot-plan',forged)
    value=json.loads((forged/'plan.json').read_text());value['writes'][3]['first_sector']=24576
    (forged/'plan.json').write_text(json.dumps(value))
    refused(lambda:host.plan(forged/'plan.json',allocation,image,record,work/'refused-region'))
    store=host.ProtectedDirectory(work/'update-plan')
    try:
        for index,session in enumerate(sessions):
            first,blocks=session['first'],session['blocks']
            media=work/f'session-{index}.bin';media.write_bytes(card[first*512:(first+blocks)*512])
            read=dict(base,nonce=f'{0xc0+2*index:02x}'*32,sectors=sectors,first=first,blocks=blocks,
                      allocation=session['allocation_sha256'])
            assert host.card_range(read)==session['card_range']
            run=Session(read,media)
            with host.ProtectedDirectory(work/f'session-{index}-backup',True) as backup:
                _,manifest=host.backup(host.Client(run.exchange,read),backup)
            run.close();assert manifest['sha256']==session['backup_sha256']
            write=dict(read,mode=1,nonce=f'{0xc1+2*index:02x}'*32)
            assert host.allocation_assigns(store.json(session['allocation']),write)
            if index==1:
                other=sessions[0]
                with host.ProtectedDirectory(work/f'session-{index}-backup') as old,host.ProtectedDirectory(work/'refused-session',True) as out:
                    refused(lambda:host._prepare_files(out,old,store.path/session['candidate'],write,other))
            with host.ProtectedDirectory(work/f'session-{index}-backup') as old,host.ProtectedDirectory(work/f'session-{index}-update',True) as out:
                update=host._prepare_files(out,old,store.path/session['candidate'],write,session)
            run=Session(update['grant'],media)
            with host.ProtectedDirectory(work/f'session-{index}-update') as out:host.apply(host.Client(run.exchange,update['grant']),out)
            run.close();card[first*512:(first+blocks)*512]=run.output.read_bytes()
            assert hashlib.sha256(run.output.read_bytes()).hexdigest()==session['candidate_sha256']
    finally:store.close()
    expected=bytearray(original)
    for item in json.loads(slot_plan.read_text())['writes']:
        expected[item['first_sector']*512:(item['first_sector']+item['sectors'])*512]=(work/'slot-plan'/item['file']).read_bytes()
    assert card==expected and card[24576*512:32768*512]==original[24576*512:32768*512]
    image.write_bytes(card)
    fd,count=slots.open_image(image)
    try:assert slots.public(slots.inspect(fd,count))['selected']=='A'
    finally:os.close(fd)
    passed.append('each planned session runs a connected backup and update; slot A verifies and no other sector changes')
    again=host.plan(slot_plan,allocation,image,record,work/'repeat-plan')
    assert not again['sessions'] and [s['name'] for s in again['skipped']]==['payload','header','boot0','monitor-package']
    passed.append('ranges that already hold their planned bytes are skipped')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    original = (root/'checks/sunxi-mmc.c').read_text()
    model = original[original.index('struct model {'):original.index('static int barrier(void *ctx)')]
    (evidence/'mmc-card-model.h').write_text(model)
    provider = (root/'checks/a733-mmc-platform.c').read_text()
    old = 'storage_leave,storage_retain,storage_read,storage_write,storage_barrier,storage_outside'
    assert provider.count(old) == 1
    provider = provider.replace(old, 'storage_leave,storage_retain,update_read,update_write,storage_barrier,storage_outside')
    old = 'storage_read_fifo,storage_write_fifo'
    assert provider.count(old) == 1
    provider = provider.replace(old, 'storage_read_fifo,update_write_fifo')
    (evidence/'storage-provider-model.h').write_text(provider)
    (evidence/'model-extraction.json').write_text(json.dumps(dict(
        original_card=hashlib.sha256(original.encode()).hexdigest(),
        extracted_card=hashlib.sha256(model.encode()).hexdigest(),
        original_platform=hashlib.sha256((root/'checks/a733-mmc-platform.c').read_bytes()).hexdigest(),
        transformed_platform=hashlib.sha256(provider.encode()).hexdigest(),
        change='Only initializer MMIO and FIFO-write callbacks select the write-capable card model; original platform cases are not run.'), indent=2))
    compiler = shutil.which('clang-21') or shutil.which('clang')
    assert compiler
    sources = ['drivers/storage/a733_storage_update.c','drivers/storage/storage_digest.c',
               'drivers/storage/storage_update_usb.c','drivers/storage/a733_mmc_platform.c',
               'drivers/storage/sunxi_mmc.c','drivers/clock/a733_clock_leases.c',
               'drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c',
               'drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c',
               'drivers/pinctrl/a733_pin_tables.c','checks/a733-storage-update.c']
    flags = [compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-Iinclude','-Idrivers/storage','-Ichecks','-I'+str(evidence)]
    kernel = args.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    native = [compiler,'--target=aarch64-none-elf','-std=c11','-O2','-ffreestanding','-fno-builtin',
              '-Wall','-Wextra','-Werror','-fstack-usage','-Iinclude','-Idrivers/storage','-Iruntime/include',
              '-I'+str(kernel/'include'),'-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),
              '-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetSchedulerState=1',
              '-DINCLUDE_xTaskGetCurrentTaskHandle=1']
    native_sources = sources[:5]+['drivers/storage/a733_mmc_platform_native.c']
    commands = []
    def run(command, name):
        command = [str(x) for x in command]
        commands.append(command)
        result = subprocess.run(command,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (evidence/name).write_text(result.stdout)
        (evidence/'commands.json').write_text(json.dumps(commands,indent=2))
        if result.returncode:
            print(result.stdout)
            raise SystemExit(result.returncode)
        return result.stdout
    for i, source in enumerate(sources):
        run(flags+['-M','-MF',evidence/f'host-{i}.d',source],f'host-dependencies-{i}.log')
    for i, source in enumerate(native_sources):
        run(native+['-M','-MF',evidence/f'native-{i}.d',source],f'native-dependencies-{i}.log')
    paths = {root/s for s in sources+native_sources+['checks/a733-mmc-sdc3.c','checks/sunxi-mmc-write.c','checks/a733-mmc-platform.py','checks/a733-mmc-sdc3.py','checks/sunxi-mmc-write.py']+['checks/a733-storage-update.py','tools/storage-update.py','tools/boot-slots.py','tools/check-a733-smp.py','checks/sunxi-mmc.c','checks/a733-mmc-platform.c']}
    paths.add(Path(compiler).resolve())
    for dep in evidence.glob('*.d'):
        paths.update((root/p).resolve() for p in dep.read_text().replace('\\\n',' ').split(':',1)[1].split())
    records=[]
    for path in sorted(paths):
        data=path.read_bytes();sha=hashlib.sha256(data).hexdigest();target=evidence/'inputs'/sha
        target.parent.mkdir(exist_ok=True)
        if not target.exists():target.write_bytes(data)
        records.append(dict(path=str(path),sha256=sha,snapshot=str(target.relative_to(evidence))))
    (evidence/'inputs.json').write_text(json.dumps(records,indent=2))
    binary=evidence/'storage-server'
    run(flags+sources+['-o',binary],'host-build.log')
    spec=importlib.util.spec_from_file_location('storage_update_host',root/'tools/storage-update.py')
    host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    for line in run([binary,'--digest'],'digest.log').splitlines():
        size,got=line.split();assert hashlib.sha256(bytes(i%256 for i in range(int(size)))).hexdigest()==got
    passed=[]
    fixture=evidence/'fixtures';fixture.mkdir()
    initial=bytes((i*17+9)%256 for i in range(1024));candidate=bytes((i*31+11)%256 for i in range(1024))
    (fixture/'initial.bin').write_bytes(initial);(fixture/'candidate.bin').write_bytes(candidate)
    base=dict(mode=0,kind=1,host=0,nonce='01'*32,cid=[0x12345678+i for i in range(4)],
              sectors=33554432,first=2,blocks=2,deadline_us=9000000,backup='00'*32,candidate='00'*32,
              allocation='a1'*32,recovery='b2'*32)
    count=0
    class Session:
        def __init__(self,g,media,mode='normal'):
            nonlocal count
            count+=1;self.prefix=f'session-{count:02d}';self.g=g;self.output=fixture/(self.prefix+'-media.bin')
            grant=fixture/(self.prefix+'-grant.bin');grant.write_bytes(host.grant_encode(g))
            self.log=(evidence/(self.prefix+'.log')).open('wb')
            self.process=subprocess.Popen([str(binary),str(grant),str(media),str(self.output),mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log)
        def exchange(self,p):
            self.process.stdin.write(struct.pack('<I',len(p))+p);self.process.stdin.flush()
            header=self.process.stdout.read(4)
            if len(header)!=4:raise RuntimeError('model transport ended')
            n,=struct.unpack('<I',header)
            if not n:raise RuntimeError('model retained session')
            answer=self.process.stdout.read(n)
            if len(answer)!=n:raise RuntimeError('model transport truncated')
            return answer
        def close(self):
            self.process.stdin.close();self.process.stdout.close()
            result=self.process.wait(timeout=10);self.log.close();assert result==0,self.prefix
    session=Session(base,fixture/'initial.bin')
    with host.ProtectedDirectory(fixture/'backup',True) as store:
        host.backup(host.Client(session.exchange,base),store)
    session.close();assert session.output.read_bytes()==initial
    passed.append('actual card/platform/provider backup to protected fsynced host bytes')
    def prepared(path,prior,source,nonce):
        g=dict(base,mode=1,nonce=nonce*32)
        with host.ProtectedDirectory(prior) as old,host.ProtectedDirectory(path,True) as out:
            return host._prepare_files(out,old,source,g)
    update=prepared(fixture/'update',fixture/'backup',fixture/'candidate.bin','02')
    session=Session(update['grant'],fixture/'initial.bin')
    with host.ProtectedDirectory(fixture/'update') as store:
        host.apply(host.Client(session.exchange,update['grant']),store)
        assert [store.json(f'intent-{i:08d}.json')['sha256'] for i in range(2)]==[hashlib.sha256(candidate[i*512:i*512+512]).hexdigest() for i in range(2)]
        assert store.json('complete.json')['readback_sha256']==hashlib.sha256(candidate).hexdigest()
    session.close();assert session.output.read_bytes()==candidate
    passed.append('actual bounded CMD24 writes and card readback with durable host intent/result')
    current=dict(base,nonce='03'*32)
    session=Session(current,fixture/'candidate.bin')
    with host.ProtectedDirectory(fixture/'current-backup',True) as store:host.backup(host.Client(session.exchange,current),store)
    session.close()
    restore=prepared(fixture/'restore',fixture/'current-backup',fixture/'backup/backup.bin','04')
    session=Session(restore['grant'],fixture/'candidate.bin')
    with host.ProtectedDirectory(fixture/'restore') as store:host.apply(host.Client(session.exchange,restore['grant']),store)
    session.close();assert session.output.read_bytes()==initial
    passed.append('explicit fresh-session restoration from protected original bytes after current-state backup')
    for mode in ('uncertain','loss'):
        item=prepared(fixture/mode,fixture/'backup',fixture/'candidate.bin', '05' if mode=='uncertain' else '06')
        session=Session(item['grant'],fixture/'initial.bin',mode)
        client=host.Client(session.exchange,item['grant'])
        with host.ProtectedDirectory(fixture/mode) as store:
            try:host.apply(client,store);assert False
            except RuntimeError:pass
            assert client.failed
            try:client.call(1);assert False
            except ValueError:pass
            assert (store.path/'intent-00000000.json').exists() and not (store.path/'result-00000000.json').exists()
        # A failed word ends its burst; a loss latched mid-burst stops the next admitted step.
        written=4 if mode=='uncertain' else 512
        session.close();assert session.output.read_bytes()==candidate[:written]+initial[written:]
    passed.append('uncertain FIFO write and preempting USB loss retain source/range with no retry')
    # One-sector WRITE requests leave room for 500 ms of card programming per
    # sector plus readback; programming beyond the request deadline retains.
    item=prepared(fixture/'slow',fixture/'backup',fixture/'candidate.bin','09')
    session=Session(item['grant'],fixture/'initial.bin','slow')
    with host.ProtectedDirectory(fixture/'slow') as store:
        host.apply(host.Client(session.exchange,item['grant']),store)
        assert store.json('complete.json')['readback_sha256']==hashlib.sha256(candidate).hexdigest()
        assert [store.json(f'intent-{i:08d}.json')['bytes'] for i in range(2)]==[512,512]
    session.close();assert session.output.read_bytes()==candidate
    item=prepared(fixture/'stalled',fixture/'backup',fixture/'candidate.bin','0a')
    session=Session(item['grant'],fixture/'initial.bin','stalled');client=host.Client(session.exchange,item['grant'])
    with host.ProtectedDirectory(fixture/'stalled') as store:
        try:host.apply(client,store);assert False
        except RuntimeError:pass
        assert client.failed and (store.path/'intent-00000000.json').exists() and not (store.path/'result-00000000.json').exists()
    session.close();assert session.output.read_bytes()==candidate[:512]+initial[512:]
    passed.append('500 ms programming per one-sector write completes with readback; a longer stall retains the uncertain write')
    item=prepared(fixture/'wrong-commitment',fixture/'backup',fixture/'candidate.bin','08')
    session=Session(item['grant'],fixture/'initial.bin')
    def corrupt_write(packet):
        if struct.unpack_from('<I',packet,36)[0]==4:
            packet=packet[:-1]+bytes([packet[-1]^1])
        return session.exchange(packet)
    with host.ProtectedDirectory(fixture/'wrong-commitment') as store:
        client=host.Client(corrupt_write,item['grant'])
        try:host.apply(client,store);assert False
        except RuntimeError:pass
        assert client.failed
    session.close();assert session.output.read_bytes()==initial
    passed.append('candidate commitment mismatch refuses before first write')
    # No valid write admission can follow a failed host fsync.
    item=prepared(fixture/'fsync-fail',fixture/'backup',fixture/'candidate.bin','07')
    session=Session(item['grant'],fixture/'initial.bin');client=host.Client(session.exchange,item['grant'])
    real_fsync=host.os.fsync
    with host.ProtectedDirectory(fixture/'fsync-fail') as store:
        def reject(fd):raise OSError('injected fsync failure')
        host.os.fsync=reject
        try:
            try:host.apply(client,store);assert False
            except OSError:pass
        finally:host.os.fsync=real_fsync
        assert client.failed and not (store.path/'intent-00000000.json').exists()
    session.close();assert session.output.read_bytes()==initial
    passed.append('host fsync failure refuses mutation before seal/write admission')
    # Native request validation precedes commands; no old protocol cases replayed.
    for name,change in [('range',lambda p:struct.pack_into('<Q',p,40,3)),('nonce',lambda p:p.__setitem__(0,99)),('sequence',lambda p:struct.pack_into('<I',p,32,2))]:
        session=Session(base,fixture/'initial.bin')
        packet=bytearray(bytes.fromhex(base['nonce'])+struct.pack('<IIQII',1,2,0,0,0)+bytes(32));change(packet)
        try:session.exchange(packet);assert False
        except RuntimeError:pass
        session.close();assert session.output.read_bytes()==initial
    bad=dict(base,cid=[0,1,2,3]);session=Session(bad,fixture/'initial.bin','wrong-card');session.close()
    passed.append('wrong card, range, nonce and sequence refuse commands')
    # Files cannot be supplied through symlinks or altered after their recorded hash.
    for kind in ('symlink','corrupt','truncated'):
        directory=fixture/('bad-'+kind)
        with host.ProtectedDirectory(directory,True) as store:
            if kind=='symlink':os.symlink(fixture/'backup/backup.bin',directory/'backup.bin')
            else:
                data=(b'x'+initial[1:]) if kind=='corrupt' else initial[:-1]
                fd=store.create('backup.bin');host.write_all(fd,data);store.seal('backup.bin',fd,len(data),host.digest(data))
            try:store.open('backup.bin',1024,host.digest(initial));assert False
            except (OSError,ValueError):pass
    passed.append('actual protected-file symlink, corruption and truncation refusal')
    planned_card_update(host,fixture,base,Session,passed)
    for i,source in enumerate(['checks/a733-mmc-platform.c','checks/a733-mmc-sdc3.c','checks/sunxi-mmc-write.c']):
        run(flags+['-c',source,'-o',evidence/f'embedding-{i}.o'],f'embedding-{i}.log')
    for i,source in enumerate(native_sources):run(native+['-c',source,'-o',evidence/(Path(source).stem+'.o')],f'native-build-{i}.log')
    assert all(hashlib.sha256(Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in records)
    (evidence/'result.json').write_text(json.dumps(dict(result='pass',physical=False,qualification_created=False,
        original_mmc_suites_replayed=False,checks=passed,sha256_vectors=8),indent=2))
    print('\n'.join('PASS: '+x for x in passed))


if __name__=='__main__':main()
