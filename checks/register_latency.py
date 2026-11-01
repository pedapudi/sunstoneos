"""Deferred masked register effects, using explicit fixture counter units.

Reads never advance time. The caller keeps busy flags and stale data in its
visible register word until apply() completes. Schedule separate effects for
independent completion signals, such as UPDATE clearing and LOCK asserting.
Time is a monotonic unsigned 64-bit counter; elapsed subtraction permits one
wrap during an event. The C fixtures use the same rules in register_latency.h.
"""
from dataclasses import dataclass


@dataclass
class RegisterLatency:
    start: int
    delay: int
    mask: int
    value: int
    pending: bool = True
    stuck: bool = False

    def apply(self, now, visible):
        if self.pending and not self.stuck and ((now - self.start) & ((1 << 64) - 1)) >= self.delay:
            self.pending = False
            return (visible & ~self.mask) | (self.value & self.mask)
        return visible


class PostedWrites:
    """CPU-side posted stores, completed explicitly by the fixture's DSB hook.

    This is an allowed delayed-transaction scenario, not a peripheral timing
    specification. DMB orders stores without completing them. complete() ends
    the architectural transactions; endpoint effects still need RegisterLatency
    or device-specific status. Device-nGnRE may acknowledge before application.
    """

    def __init__(self, words):
        self.words = dict(words)
        self.pending = []

    def write(self, address, value):
        self.pending.append((address, value))

    def read(self, address):
        return self.words.get(address, 0)

    def complete(self):
        completed = self.pending
        self.pending = []
        for address, value in completed:
            self.words[address] = value
        return completed
