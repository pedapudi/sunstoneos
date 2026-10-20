#!/usr/bin/env python3
"""Modeled TYPEC24 hardware for the exact composed DisplayPort ELF.

Only electrical/MMIO/partner behavior is modeled. The consumer supplies the
actual native provider/task state and dispatches real provider functions. Source
anchors and earlier protocol matrix: docs/typec-pd-sources.json and
~/.cache/sunstoneos/evidence/typec-pd-20261020/.
"""
import copy
import zlib


class TypeCHardware:
    """Lower MMIO model; read/write return None/False for another owner."""
    TWI0 = 0x07083000
    TWI1 = 0x07084000
    GPIO = 0x07025010

    def __init__(self, now_us, *, polarity=1):
        assert polarity in (1, 2)
        self.now_us = now_us
        self.polarity = polarity
        self.events = []
        self.gpio = 0
        self.cc_interrupt = 0
        self.last_status = 0
        self.registers = {1: 0x80, 0x0a: 255, 0x0e: 255, 0x0f: 1}
        # Read-only modeled supplies: DCDC1/DC1SW2 at3.3V, DCDC2 at0.8V,
        # CLDO5 at1.8V. The actual native PMIC decoder consumes these bytes.
        self.pmic = {0x0e: 3, 0x10: 3, 0x11: 16, 0x12: 23,
                     0x13: 30, 0x21: 128, 0x33: 13}
        self.bus = {
            self.TWI0: dict(control=0x40, status=0xf8, data=0, wire=0,
                            receiving=False, selected=0, due=0, stop=False),
            self.TWI1: dict(control=0, status=0xf8, data=0, wire=0,
                            receiving=False, selected=0, due=0, stop=False),
        }
        self.rx = bytearray()
        self.tx = bytearray()
        self.peer_id = 0
        self.transmissions = []
        self.goodcrc = 0
        self.ack_reads = 0
        self.nack_reads = 0
        self.completed_stops = 0
        self.configure_ack = False
        self.cc_samples = []
        self.safe0_samples = []
        self.enable_times = []

    def export(self):
        return copy.deepcopy({key: value for key, value in vars(self).items()
                              if key != 'now_us'})

    def restore(self, state):
        for key, value in copy.deepcopy(state).items():
            assert key != 'now_us'
            setattr(self, key, value)

    def status(self):
        switch = self.registers.get(2, 0)
        measure = self.registers.get(4, 0)
        if measure & 0x40:
            voltage = 5000 if self.gpio & 256 else 0
            threshold = ((measure & 63) + 1) * 420
        else:
            selected = 1 if switch & 4 else 2 if switch & 8 else 0
            voltage = 500 if selected == self.polarity else 3300 if selected else 0
            threshold = ((measure & 63) + 1) * 42
        value = (0x80 if self.gpio & 256 else 0) | (0x20 if voltage > threshold else 0)
        self.cc_interrupt |= (value ^ self.last_status) & 0xa0
        self.last_status = value
        return value

    def packet(self, header, objects=()):
        assert ((header >> 12) & 7) == len(objects)
        payload = header.to_bytes(2, 'little') + b''.join(v.to_bytes(4, 'little') for v in objects)
        self.rx.extend(b'\xe0' + payload + (zlib.crc32(payload) & 0xffffffff).to_bytes(4, 'little'))

    def response(self, objects):
        self.packet(15 | 0x40 | (self.peer_id << 9) | (len(objects) << 12), objects)
        self.peer_id = (self.peer_id + 1) & 7

    def transmit(self):
        raw = bytes(self.tx)
        assert raw[:4] == b'\x12\x12\x12\x13'
        length = raw[4] & 31
        assert raw[4] & 0xe0 == 0x80 and len(raw) == length + 9
        assert raw[-4:] == b'\xff\x14\xfe\xa1'
        header = int.from_bytes(raw[5:7], 'little')
        count = (header >> 12) & 7
        assert length == 2 + count * 4 and header & 0x1e0 == 0x160
        objects = [int.from_bytes(raw[7+i*4:11+i*4], 'little') for i in range(count)]
        self.transmissions.append(dict(time_us=self.now_us(), header=header, objects=objects))
        # The modeled sink receives the packet and supplies a complete CRC-good
        # GoodCRC. Native software verifies its bytes and never consumes this log.
        self.packet(1 | 0x40 | (header & 0xe00))
        self.goodcrc += 1
        kind = header & 31
        if kind == 1:
            assert objects == [(1 << 26) | (100 << 10) | 50]
            self.packet(2 | 0x40 | (self.peer_id << 9) | (1 << 12),
                        [0x10000000 | (50 << 10) | 50])
            self.peer_id = (self.peer_id + 1) & 7
        elif kind == 15:
            assert objects and objects[0] & 0x80e0 == 0x8000
            request = objects[0]
            command = request & 31
            response = [request | 0x40]
            if command == 1:
                assert request >> 16 == 0xff00
                response += [1 << 26, 0, 0]
            elif command == 2:
                response += [0xff010000]
            elif command == 3:
                response += [(4 << 16) | 0x45]  # Receptacle UFP_D, DP signaling, pin C.
            elif command == 4:
                assert (request >> 8) & 7 == 1
            elif command == 16:
                assert objects[1:] == [1]
                response += [0x82]  # UFP_D connected, HPD high, no IRQ.
            elif command == 17:
                assert objects[1:] == [0x406]
                self.configure_ack = True
            else:
                raise AssertionError(('unexpected VDM', objects))
            self.response(response)
        else:
            assert count == 0 and kind in (3, 6)  # Accept and PS_RDY.
        self.tx.clear()

    def fusb_read(self, reg):
        if reg == 0x40:
            value = self.status()
            sample = [self.now_us(), self.registers.get(2, 0), self.registers.get(4, 0), value]
            self.cc_samples.append(sample)
            if sample[2] & 0x40:
                self.safe0_samples.append(sample)
            return value
        if reg == 0x41:
            return 8 | (0x20 if not self.rx else 0)
        if reg == 0x42:
            self.status()
            value = self.cc_interrupt
            self.cc_interrupt = 0
            return value
        if reg == 0x3d:
            return self.polarity << 3
        if reg in (0x3e, 0x3f):
            return 0
        if reg == 0x43:
            assert self.rx, 'native FIFO read exceeded complete modeled packets'
            value = self.rx[0]
            del self.rx[0]
            return value
        return self.registers.get(reg, 0)

    def fusb_write(self, reg, value):
        self.status()
        if reg == 0x0c and value == 1:
            self.registers = {1: 0x80, 0x0a: 255, 0x0e: 255, 0x0f: 1}
            self.cc_interrupt = 0
        elif reg == 0x43:
            self.tx.append(value)
            assert len(self.tx) <= 40
            if len(self.tx) >= 5 and len(self.tx) == 9 + (self.tx[4] & 31):
                self.transmit()
        else:
            # FIFO flush bits self-clear in the modeled controller.
            self.registers[reg] = value & ~(0x40 if reg == 6 else 4 if reg == 7 else 0)
        self.status()
        self.events.append(['fusb', reg, value, self.now_us()])

    def read(self, address):
        if address == self.GPIO:
            self.status()
            active = not (self.registers.get(6, 0) & 0x20) and bool(
                self.cc_interrupt & ~self.registers.get(0x0a, 255))
            return self.gpio | (0 if active else 512)
        base = address & ~4095
        if base not in self.bus:
            return None
        bus = self.bus[base]
        offset = address - base
        if offset not in (8, 0x0c, 0x10):
            return None
        if offset == 0x0c:
            if self.now_us() < bus['due']:
                return 0x58 if bus['stop'] else 0x40
            if bus['stop']:
                bus.update(wire=0, receiving=False, control=0x40,
                           status=0xf8, stop=False)
                self.completed_stops += 1
            return bus['control']
        return bus['data' if offset == 8 else 'status']

    def write(self, address, value):
        if address == self.GPIO:
            if value & 256 and not self.gpio & 256:
                assert self.safe0_samples and not self.safe0_samples[-1][3] & 0xa0
                assert not self.safe0_samples[-1][1] & 12
                self.enable_times.append(self.now_us())
            self.events.append(['gpio', address, value, self.now_us()])
            self.gpio = value
            self.status()
            return True
        base = address & ~4095
        if base not in self.bus:
            return False
        bus = self.bus[base]
        offset = address - base
        if offset == 8:
            bus['data'] = value
            return True
        if offset != 0x0c:
            return False
        if not value & 8:
            bus['control'] = value
            return True
        assert value in (0x48, 0x4c, 0x58, 0x68)
        if value == 0x68:
            assert bus['wire'] in (0, 3)
            bus['status'] = 8 if not bus['wire'] else 0x10
            bus['wire'] = 1
            bus['receiving'] = False
        elif value == 0x58:
            bus.update(control=value, stop=True, due=self.now_us()+25)
            return True
        elif bus['receiving']:
            selected = bus['selected']
            bus['data'] = self.pmic.get(selected, 0) if base == self.TWI0 else self.fusb_read(selected)
            bus['status'] = 0x50 if value & 4 else 0x58
            self.ack_reads += bool(value & 4)
            self.nack_reads += not bool(value & 4)
        elif bus['wire'] == 1:
            expected = 0x6c if base == self.TWI0 else 0x44
            assert bus['data'] in (expected, expected | 1)
            bus['receiving'] = bool(bus['data'] & 1)
            bus['status'] = 0x40 if bus['receiving'] else 0x18
            bus['wire'] = 2
        elif bus['wire'] == 2:
            bus['selected'] = bus['data']
            bus['status'] = 0x28
            bus['wire'] = 3
        else:
            assert bus['wire'] == 3 and base == self.TWI1
            self.fusb_write(bus['selected'], bus['data'])
            bus['status'] = 0x28
        bus.update(control=0x48 | (value & 4), due=self.now_us()+90)
        return True
