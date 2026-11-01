"""Model of the CPU PLL block for the DDR wrapper checks (library).

The three PLL words (CPU-L 0x08871000, CPU-B 0x08872000, DSU 0x08873000) and
muxes (+0x1c) start at recorded boot0 values. N and the old lock bit remain
visible while UPDATE is pending. UPDATE completes after update_reads * step
counter ticks; LOCK asserts lock_reads * step ticks later. These are fixture
latencies within the vendor bounds, not physical measurements. Reads alone do
not complete either event. stuck_update, stuck_lock and stuck_mux suppress the
corresponding completion. lock_glitch injects one clear read after the first
new locked observation. The wrapper's counter routine is replaced by a fake
24 MHz counter that starts at `start` and advances `step` ticks per call
until it reaches `stop` (if given). Every access is logged with the counter
value, so a check can verify the order of writes, the waits and the final
words. No hardware is involved.
"""
import struct
from register_latency import RegisterLatency
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1

BASE = 0x08870000
UPDATE, LOCK, SOURCE = 1 << 26, 1 << 28, 7 << 24
DOMAINS = {"cpu-l": 0x08871000, "cpu-b": 0x08872000, "dsu": 0x08873000}
BOOT0 = {0x08871000: 0xf8802700, 0x0887101c: 0x03000000, 0x08872000: 0xf8802700,
         0x0887201c: 0x03000000, 0x08873000: 0xf8801e00, 0x0887301c: 0x03000105}
ROM = {0x08871000: 0xf8802600, 0x0887101c: 0x03010000, 0x08873000: 0xf8802600, 0x0887301c: 0x03010105}


class ClockModel:
    def __init__(self, uc, ticks_address, words=None, update_reads=2, lock_reads=3,
                 stuck_lock=(), stuck_update=(), lock_glitch=(), stuck_mux=(), step=4, start=0x10000, stop=None):
        self.words = dict(BOOT0 if words is None else words)
        self.update_reads, self.lock_reads, self.step, self.stop = update_reads, lock_reads, step, stop
        self.stuck_lock = {DOMAINS[d] for d in stuck_lock}
        self.stuck_update = {DOMAINS[d] for d in stuck_update}
        self.lock_glitch = {DOMAINS[d] for d in lock_glitch}
        self.stuck_mux = {DOMAINS[d] + 0x1c for d in stuck_mux}
        self.pending = {}          # PLL word -> independent UPDATE and LOCK effects
        self.tick = start
        self.log = []              # (op, address, value, tick)
        self.ticks_address = ticks_address
        uc.mem_map(BASE, 0x4000)
        uc.hook_add(UC_HOOK_CODE, self._code, begin=ticks_address, end=ticks_address)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._memory, begin=BASE, end=BASE + 0x3fff)

    def _code(self, uc, address, size, data):
        uc.reg_write(UC_ARM_REG_R0, self.tick & 0xffffffff)
        uc.reg_write(UC_ARM_REG_R1, self.tick >> 32)
        if self.stop is None or self.tick < self.stop:
            self.tick += self.step
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def _memory(self, uc, access, address, size, value, data):
        assert size == 4 and address in self.words, ("CPU PLL access", hex(address), size)
        if access == UC_HOOK_MEM_WRITE_ACCESS:
            self.log.append(("w", address, value & 0xffffffff, self.tick))
            if address & 0xfff == 0:
                mux = self.words[address + 0x1c]
                assert not mux & SOURCE, ("PLL written while its mux selects it", hex(address))
                if value & UPDATE:
                    nmask = 0xff00
                    update = RegisterLatency(self.tick, self.update_reads * max(1, self.step),
                                             nmask | UPDATE | LOCK, value & nmask,
                                             stuck=address in self.stuck_update)
                    lock = RegisterLatency(self.tick, (self.update_reads + self.lock_reads) * max(1, self.step),
                                           LOCK, LOCK,
                                           stuck=address in self.stuck_lock or address in self.stuck_update)
                    self.pending[address] = [update, lock, 2 if address in self.lock_glitch else 0]
                # The multiplier is latched by UPDATE, not by the preceding write.
                value = (value & ~(LOCK | 0xff00)) | (self.words[address] & (LOCK | 0xff00))
            if address not in self.stuck_mux:
                self.words[address] = value & 0xffffffff
            return
        word = self.words[address]
        if address in self.pending:
            update, lock, glitch = self.pending[address]
            word = update.apply(self.tick, word)
            word = lock.apply(self.tick, word)
            if not update.pending and not lock.pending:
                if glitch:
                    if glitch == 1:
                        word &= ~LOCK
                    self.pending[address][2] -= 1
                else:
                    word |= LOCK
                    del self.pending[address]
            self.words[address] = word
        self.log.append(("r", address, word, self.tick))
        uc.mem_write(address, struct.pack("<I", word))

    def writes(self):
        return [(address, value) for op, address, value, _ in self.log if op == "w"]

    def check_sequence(self, domain, final_mux):
        """Order and waits of one domain's return to the ROM point."""
        pll = DOMAINS[domain]
        mux = pll + 0x1c
        entries = [e for e in self.log if e[1] in (pll, mux)]
        writes = [e for e in entries if e[0] == "w"]
        start_mux = BOOT0[mux]
        n38 = (BOOT0[pll] & ~0xff00) | (38 << 8)
        assert [(a, v) for _, a, v, _ in writes] == [
            (mux, start_mux & ~SOURCE), (pll, n38), (pll, n38 | UPDATE),
            (mux, final_mux & ~SOURCE), (mux, final_mux)], (domain, [(hex(a), hex(v)) for _, a, v, _ in writes])
        park, n_write, update, divider, select = (e[3] for e in writes)
        assert n_write - park >= 480, (domain, "20 us after parking", n_write - park)
        # Lock reads between the update request and the divider write: the
        # last three show lock and lie at least 3 us apart.
        reads = [e for e in entries if e[0] == "r" and e[1] == pll and update <= e[3] <= divider]
        locked = [e for e in reads if e[2] & LOCK and not e[2] & UPDATE][-3:]
        assert len(locked) == 3 and reads[-3:] == locked, (domain, "three lock reads")
        assert all(b[3] - a[3] >= 72 for a, b in zip(locked, locked[1:])), (domain, "3 us between lock reads")
        assert divider - locked[-1][3] >= 480, (domain, "20 us after lock")
        return select


UC_HOOK_MEM_WRITE_ACCESS = 17  # UC_MEM_WRITE in the access argument of a memory hook.
