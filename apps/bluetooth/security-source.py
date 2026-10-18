#!/usr/bin/env python3
"""Verify pinned security sources and adapt only their platform includes."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def function(text,name):
    match=re.search(r'(?m)^(?:static )?(?:bool|int) '+re.escape(name)+r'\([^;]*?\)\s*\{',text)
    if match is None:raise ValueError('Pinned function disappeared: '+name)
    start=match.start();cursor=match.end();depth=1
    while depth:
        if text[cursor]=='{':depth+=1
        elif text[cursor]=='}':depth-=1
        cursor+=1
    return text[start:cursor]+'\n'


def prepare(source,output):
    record=json.loads(Path(__file__).with_suffix('.json').read_text());source=Path(source);output=Path(output);output.mkdir(parents=True,exist_ok=True)
    raw={}
    for name,item in record['sources'].items():
        data=(source/name).read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('Pinned security source differs: '+name)
        raw[name]=data.decode()
    for name in ['ssp','keys_br']:
        text=raw['subsys/bluetooth/host/classic/'+name+'.c']
        # The policy body and original license stay unchanged. This port has
        # one owner, fixed copied buffers and no Zephyr scheduler/settings.
        text=re.sub(r'(?m)^#include [^\n]+\n','',text)
        text=re.sub(r'(?m)^#define LOG_LEVEL [^\n]+\n|^LOG_MODULE_REGISTER\([^\n]+\n','',text)
        (output/('zephyr-'+name+'.c')).write_text('#include "bt_security_port.h"\n'+text)
    text=raw['subsys/bluetooth/host/classic/br.c']
    # Its blocking key-size query is supplied by the actual earlier tagged
    # completion; keep the upstream security-level decision itself unchanged.
    (output/'zephyr-security-level.c').write_text('#include "bt_security_port.h"\n'+text[:text.index('#include')]+function(text,'bt_br_update_sec_level'))
    text=raw['include/zephyr/bluetooth/hci_types.h'];ssp=raw['subsys/bluetooth/host/classic/ssp.c']
    names=sorted(set(re.findall(r'struct (bt_hci_(?:cp|evt)_[a-z_]+)',ssp)))
    chunks=[text[:text.index('#ifndef')],'#pragma once\n']
    for name in names:
        found=re.search(r'(?ms)^struct '+re.escape(name)+r' \{.*?^\} __packed;',text)
        if found is None:raise ValueError('Pinned HCI layout disappeared: '+name)
        chunks.append(found.group()+'\n')
    identifiers=set(re.findall(r'\bBT_(?:HCI_(?:OP|ERR|NO|GENERAL|DEDICATED)_[A-Z0-9_]+|IO_[A-Z0-9_]+|LK_[A-Z0-9_]+|MITM)\b',ssp))
    definitions={}
    def define(name):
        if name in definitions:return
        found=re.search(r'(?m)^#define '+re.escape(name)+r'(?:\([^\n]*?\))?\s+[^\n]+',text)
        if found is None:raise ValueError('Pinned HCI value disappeared: '+name)
        definitions[name]=found.group()
        for dependency in re.findall(r'\bBT_[A-Z0-9_]+\b',found.group().split(None,2)[2]):
            if dependency!=name:define(dependency)
    identifiers.add("BT_HCI_ENCRYPTION_ON_BR_AES_CCM")
    for name in sorted(identifiers):define(name)
    chunks += [value+'\n' for value in definitions.values()]
    (output/'zephyr-security-wire.h').write_text('\n'.join(chunks))
    (output/'LICENSE').write_text(raw['LICENSE'])
    (output/'security-inputs.json').write_text(json.dumps(record,indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();prepare(a.source,a.output)
