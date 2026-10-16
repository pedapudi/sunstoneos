#!/usr/bin/env python3
"""Complete a diagnosed oversized FEL read by matching its pending bytes to a known bundle."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import usb.core
import usb.util

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('evidence', type=Path)
parser.add_argument('bundle', type=Path)
args = parser.parse_args()
prior = json.loads((args.evidence / 'session.json').read_text())
data = args.bundle.read_bytes()
if prior.get('complete') or prior.get('ram_input_sha256') != hashlib.sha256(data).hexdigest():
    sys.exit('The failed session and bundle do not match.')
if any('reset64' in row.get('command', []) for row in prior['transactions']):
    sys.exit('The board was executed; this recovery only handles an interrupted ROM read.')
if 'usb_bulk_recv() ERROR -7' not in prior.get('failure', ''):
    sys.exit('The recorded failure is not the diagnosed receive timeout.')
result_path = args.evidence / 'finish-read.json'
if result_path.exists():
    sys.exit('This recovery was already attempted; inspect its result before further work.')
spec = importlib.util.spec_from_file_location('access', Path(__file__).with_name('usb-access.py'))
access = importlib.util.module_from_spec(spec); spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / 'git/mimus/tools'))
import usb_guard
record = dict(transactions=[], complete=False)
def save(): result_path.write_text(json.dumps(record, indent=2) + '\n')
def receive(count):
    row = dict(read=count); record['transactions'].append(row); save()
    chunk = bytes(device.read(endpoint_in, count, timeout=2000))
    row.update(received=len(chunk), sha256=hashlib.sha256(chunk).hexdigest()); save()
    return chunk

def response():
    packet = receive(13)
    if len(packet) != 13 or packet[:4] != b'AWUS' or packet[12] != 0:
        raise RuntimeError('Unexpected AWUS response; stop.')

try:
    with open('/tmp/mimus-board.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        usb_guard.check_health()
        node = access.discover(); access.require_session(node); access.require_rom(node)
        if access.identity(node) != prior['initial_identity']:
            raise RuntimeError('The device instance changed; do not resume its transfer.')
        device = usb.core.find(idVendor=0x1f3a, idProduct=0xefe8,
                               bus=int(node.parent.name), address=int(node.name))
        if device is None: raise RuntimeError('Device missing.')
        interface = device[0][0, 0]
        endpoint_in = next(ep.bEndpointAddress for ep in interface if ep.bEndpointAddress & 0x80)
        endpoint_out = next(ep.bEndpointAddress for ep in interface if not ep.bEndpointAddress & 0x80)
        usb.util.claim_interface(device, 0)
        # This reads pending data only. No new FEL operation is issued.
        first = receive(65536)
        offset = data.find(first) if len(first) == 65536 else -1
        if offset < 0 or data.find(first, offset + 1) != -1:
            raise RuntimeError('Pending data does not uniquely match the bundle; stop.')
        record['resume_offset'] = offset; save()
        offset += len(first)
        while offset < len(data):
            count = min(512 * 1024, len(data) - offset)
            chunk = receive(count)
            if chunk != data[offset:offset + count]:
                raise RuntimeError('Pending data differs from the bundle; stop.')
            offset += count
        response()
        # The data phase is now complete. Fetch its outstanding FEL status.
        request = struct.pack('<8sIIHI10x', b'AWUC', 8, 0x0c000000, 0x11, 8)
        record['transactions'].append(dict(write_status_request=request.hex())); save()
        if device.write(endpoint_out, request, timeout=2000) != len(request):
            raise RuntimeError('Incomplete status request; stop.')
        status = receive(8)
        response()
        if len(status) != 8 or struct.unpack_from('<HHB', status) != (0xffff, 0, 0):
            raise RuntimeError(f'Unexpected FEL status: {status.hex()}')
        usb.util.release_interface(device, 0)
        usb.util.dispose_resources(device)
        record['complete'] = True; save()
        print('PASS: pending bundle tail matched; data and FEL status completed without a reset.')
except (OSError, ValueError, RuntimeError, usb.core.USBError) as error:
    record['failure'] = str(error); save(); sys.exit(f'Stopped: {error}')
