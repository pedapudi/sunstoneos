"""IDMAC model for the returning native image. CPU data becomes visible only
at the cache invalidate hook after OWN closure and all reset/idle waits.
This tests software ordering; it cannot establish physical cache coherence.
"""
import struct

BASE = 0x04020000
AREA = 0x42100000
OWN, CH, FD, LD, DIC = 1 << 31, 16, 8, 4, 2


class Idmac:
    def dma_init(self):
        self.dma = None; self.cache_events = []; self.dma_reads = []
        self.reset_reads = {}; self.idle_reads = 0; self.cleaned = None
        self.dma_poll = 0; self.idst_done_reads = 0

    def dma_cache(self, operation, buffer, size):
        assert not buffer & 63 and not size & 63 and 512 <= size <= 131072
        if operation == 0:
            assert self.dma is None and not self.regs.get(BASE + 0x80, 0)
            self.cleaned = (buffer, size); self.cache_events.append(('clean', buffer, size))
            if self.variant.startswith('chain-'):
                field, value = {'chain-next': (12, 0), 'chain-flags': (0, OWN | FD | LD),
                                'chain-buffer': (8, (buffer + 64) >> 2), 'chain-size': (4, size + 512),
                                'chain-spare': (16 * ((size + 4095) // 4096), OWN)}[self.variant]
                self.write(AREA + field, value)
            if self.variant == 'cache-before-fail': return 0
        else:
            assert operation == 2 and self.dma is not None
            assert self.dma['complete'] and not self.regs.get(BASE + 0x80, 0)
            assert not self.regs.get(BASE, 0) & 0x27 and not self.idle_reads
            assert not any(self.read(a) & OWN for a in self.dma['descriptors'])
            assert (buffer, size) == self.cleaned
            if self.variant == 'cache-after-fail': return 0
            self.cpu.mem_write(buffer, self.dma['data'])
            self.cache_events.append(('invalidate', buffer, size)); self.dma_reads.append(self.dma)
            self.dma = None; self.cleaned = None
        return 1

    def dma_read_progress(self, a):
        if a in self.reset_reads:
            self.reset_reads[a] -= 1
            if not self.reset_reads[a]:
                self.regs[a] &= ~(1 if a == BASE + 0x80 else 7)
                del self.reset_reads[a]
        if a == BASE + 0x88 and self.idle_reads:
            if self.variant != 'idle-stuck': self.idle_reads -= 1
            self.regs[a] = (self.regs.get(a, 0) & ~0x1e000) | (0x2000 if self.idle_reads else 0)
        if not self.dma: return
        if a == BASE + 0x38 and not self.dma['complete']:
            self.dma_poll += 1
            if self.variant in ('crc-read', 'crc-read-stall-reset'):
                self.regs[a] |= 1 << 7
            elif self.variant == 'idst-error': self.regs[BASE + 0x88] |= 1 << 2
            elif self.dma_poll >= 3 and self.variant != 'dma-stall':
                self.dma['complete'] = True; self.fifo = self.total
                self.regs[a] |= 8 | ((1 << 14) if self.data_command == 18 else 0)
                self.regs[BASE + 0x88] = 0 if self.variant == 'no-idst-receive' else 2
                if self.variant != 'own-stuck':
                    for d in self.dma['descriptors']: self.write(d, self.read(d) & ~OWN)
        if a == BASE + 0x48: self.regs[a] = min(512, self.total)
        if a == BASE + 0x88 and self.dma['complete']:
            self.idst_done_reads += 1
            if self.variant == 'idst-late' and self.idst_done_reads == 2: self.regs[a] |= 1 << 2

    def dma_write_mmio(self, a, v):
        if a == BASE + 0x88:
            if self.variant=='idst-clear-race' and self.dma and self.dma['complete']:
                self.regs[a]=self.regs.get(a,0)|(1<<2)
            self.trace.append(['write', a, v]); self.regs[a] = self.regs.get(a, 0) & ~v; return True
        if a == BASE + 0x80:
            self.trace.append(['write', a, v]); self.regs[a] = v
            if v == 1:
                if self.variant == 'reset-stuck' and self.dma: return True
                self.reset_reads[a] = 2
                if self.dma: self.idle_reads = 2
            elif v == 0x82:
                assert self.cleaned and not self.regs.get(BASE + 0x8c, 0)
                assert self.regs.get(BASE + 0x40) == 0x200700f8 and self.regs.get(BASE + 0x84) == AREA >> 2
            else: assert v == 0
            return True
        if a == BASE and v & 6:
            self.trace.append(['write', a, v]); self.regs[a] = v
            if not (self.final and self.variant == 'stall-reset'): self.reset_reads[a] = 2
            return True
        if a != BASE + 0x18 or not v & (1 << 9) or not self.regs.get(BASE, 0) & 32: return False
        assert not v & (1 << 10) and v & 63 in (17, 18)
        count = self.regs[BASE + 0x14]; first = self.regs[BASE + 0x1c]
        buffer, size = self.cleaned
        assert count == size and not self.regs[BASE] & (1 << 31)
        n = (count + 4095) // 4096; descriptors = []
        for i in range(n):
            d = AREA + 16 * i; flags, length, address, link = struct.unpack('<4I', self.cpu.mem_read(d, 16))
            expected = OWN | CH | DIC | (FD if not i else 0)
            if i == n - 1: expected = (expected | LD) & ~DIC
            assert (flags, length, address, link) == (expected, min(4096, count - i * 4096),
                                                       (buffer + i * 4096) >> 2, (d + 16) >> 2)
            descriptors.append(d)
        assert bytes(self.cpu.mem_read(AREA + 16 * n, 16)) == bytes(16)
        data = bytes(((((first + i // 512) * 13 + i % 512) * 37 + 11) % 256) for i in range(count))
        self.dma = dict(complete=False, descriptors=descriptors, data=data, first=first, blocks=count // 512)
        self.dma_poll = self.idst_done_reads = 0
        self.trace.append(['write', a, v]); self.commands.append([v & 63, first])
        self.regs[a] = v & ~(1 << 31); self.regs[BASE + 0x38] = 4
        self.regs[BASE + 0x20] = self.regs[BASE + 0x24] = 0x900
        self.regs[BASE + 0x48] = 0; self.total = count; self.fifo = 0; self.data_command = v & 63
        return True
