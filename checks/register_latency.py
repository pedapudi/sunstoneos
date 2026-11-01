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
