#!/usr/bin/env python3
"""Copy and verify an already-running native GPU clear over USB; never launch/reset."""
import argparse
import hashlib
from pathlib import Path
import struct
import time


def decode(header, chunk, epoch):
    words = struct.unpack('<7IQI', header)
    magic, version, state, index, count, width, height, seen_epoch, checked = words
    if (magic, version, index, width, height, seen_epoch) != (0x52555047, 1, chunk, 64, 64, epoch):
        raise ValueError('GPU response identity or dimensions differ')
    if state not in (0, 1, 2) or (count, checked) != ((64, 4096) if state == 1 else (0, 0)):
        raise ValueError('GPU response state or validated extent differs')
    return state


def decode_compute(data, submission, epoch):
    if len(data)<40:
        raise ValueError('short GPU compute response')
    magic,version,state,index,count,total,sequence,seen_epoch,checked=struct.unpack('<7IQI',data[:40])
    if (magic,version,index,total,seen_epoch)!=(0x43555047,1,submission,2,epoch):
        raise ValueError('GPU compute response identity differs')
    if state not in (0,1,2) or (count,sequence,checked)!=((32,submission+1,64) if state==1 else (0,0,0)):
        raise ValueError('GPU compute response sequence or validated extent differs')
    if len(data)!=(168 if state==1 else 40):
        raise ValueError('GPU compute response length differs')
    if state==1:
        expected=[((17+(i+1)*3)^(0x9e3779b9+(i<<16)))&0xffffffff for i in range(32)]
        if struct.unpack('<32I',data[40:])!=tuple(expected):
            raise ValueError('GPU numerical output differs')
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compute', action='store_true', help='verify both numerical submissions')
    parser.add_argument('--bus', type=int, required=True)
    parser.add_argument('--address', type=int, required=True)
    parser.add_argument('--epoch', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=2)
    args = parser.parse_args()
    if args.timeout <= 0 or args.timeout > 30 or args.output.exists():
        parser.error('use a fresh output path and timeout in (0,30] seconds')
    import usb.core
    import usb.util
    device = usb.core.find(idVendor=0x1f3a, idProduct=0xefe8, bus=args.bus, address=args.address)
    if device is None:
        raise RuntimeError('selected running device is absent')
    if (usb.util.get_string(device, device.iProduct), usb.util.get_string(device, device.iSerialNumber)) != ('sunstoneos native GPU', 'sunstoneos-a733-gpu'):
        raise RuntimeError('selected USB descriptor is not the GPU application')
    # The running image configures the interface. No reset or configuration write.
    usb.util.claim_interface(device, 0)
    try:
        end = time.monotonic() + args.timeout
        pixels = bytearray()
        for chunk in range(2 if args.compute else 64):
            while True:
                remain = end - time.monotonic()
                if remain <= 0:
                    raise TimeoutError('bounded GPU observation expired')
                timeout = max(1, int(remain * 1000))
                device.write(0x02, struct.pack('<I', chunk), timeout=timeout)
                data = bytes(device.read(0x81, 296, timeout=timeout))
                if len(data) < 40:
                    raise ValueError('short GPU response')
                state = decode_compute(data, chunk, args.epoch) if args.compute else decode(data[:40], chunk, args.epoch)
                if state == 2:
                    raise RuntimeError('GPU application retained a failure')
                if state == 0:
                    if len(data) != 40:
                        raise ValueError('pending response includes output')
                    time.sleep(min(0.01, max(0, end-time.monotonic())))
                    continue
                if not args.compute and (len(data) != 296 or data[40:] != bytes.fromhex('2080c0ff') * 64):
                    raise ValueError('GPU output differs from the fixed RGBA8 clear')
                pixels.extend(data[40:])
                break
        with args.output.open('xb') as output:
            output.write(pixels)
        kind='two 32-word numerical submissions' if args.compute else '4096 RGBA8 pixels'
        print(f'Copied and verified {kind}; SHA-256 {hashlib.sha256(pixels).hexdigest()}')
    finally:
        usb.util.release_interface(device, 0)
        usb.util.dispose_resources(device)


if __name__ == '__main__':
    main()
