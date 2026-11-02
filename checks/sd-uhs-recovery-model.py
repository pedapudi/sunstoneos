#!/usr/bin/env python3
"""Host model of the card-state recovery paths for the UHS-I voltage switch (docs/designs/sd-uhs-recovery.md).

The model has a card (signalling level, state, timing), a host (PF bank level, clock gate) and one rule from the SD Physical
Layer Specification 9.10 (section 3.9.4): once the card has switched to 1.8 V signalling it stays there through CMD0 until VDD
is cycled, and then works with SDR12 timing. It runs the switch lifetime step by step, ends the session (a watchdog reset or a
retained fault) after every prefix of the steps, starts a fresh session with the resolution algorithm of the design note, and
requires the card to end in a state the session can use, with no command sent while the bank is at 1.8 V and the card still
signals at 3.3 V (the card would drive 3.3 V into a bank rated for 1.1 x 1.8 V; datasheet Table 5-3).

Two card behaviours are not in the simplified specification (its sections 4.2.4.2 and 4.2.4.3 and Table 4-2 are blank) and are
unmeasured, so every case runs for both values:
  K1  a host at 3.3 V reads a card that signals at 1.8 V (the card's high level is within the host's input range);
  K2  a card that already signals at 1.8 V answers ACMD41 with S18R set with S18A set.
A card that does not honour CMD0 at 1.8 V ("stuck" below) is the one the algorithm cannot recover; the model shows that the
card supply cycle (DC1SW2) does, when the supply is held off long enough to discharge, and when it is not, the card is
unchanged and the next hold is longer.
"""
import itertools
import sys

sys.dont_write_bytecode = True

V33, V18 = '3.3', '1.8'


class Violation(Exception):
    pass


class Card:
    def __init__(self, stuck=False, uhs=True):
        self.uhs = uhs                # supports 1.8 V signalling (S18A at 3.3 V)
        self.sig, self.state, self.timing = V33, 'idle', 'hs'
        self.stuck = stuck            # does not answer at 1.8 V after losing the switch handshake: needs a supply cycle
        self.rca = None

    def power_cycle(self):
        self.sig, self.state, self.timing, self.rca = V33, 'idle', 'hs', None


class Host:
    def __init__(self, card, k1, k2, bank=V33):
        self.card, self.k1, self.k2 = card, k1, k2
        self.bank, self.clock_on, self.gated_ms = bank, True, 0
        self.log = []
        self.s18r = False

    # electrical rule
    def _visible(self):
        c = self.card
        if c.state == 'sw_pending':
            return False                                  # the card holds CMD and DAT low until the clock stops
        if self.bank == c.sig:
            return True
        if c.sig == V18 and self.bank == V33:
            return self.k1
        return False                                      # bank 1.8 V, card 3.3 V: the card does not parse the host's levels

    def cmd(self, op):
        c = self.card
        if self.bank == V18 and c.sig == V33:
            raise Violation(f'{op} sent with the bank at 1.8 V and the card signalling 3.3 V')
        if not self.clock_on or not self._visible():
            return None
        if c.stuck and c.sig == V18 and op in ('CMD0', 'CMD8', 'ACMD41'):
            return None
        if op == 'CMD0':
            c.state, c.timing = 'idle', ('sdr12' if c.sig == V18 else 'hs')
            return 'ok'
        if op == 'CMD8':
            return 'R7' if c.state == 'idle' else None
        if op == 'ACMD41':
            if c.state not in ('idle', 'ready'):
                return None
            c.state = 'ready'
            s18a = (c.sig == V33 and c.uhs) or (c.sig == V18 and self.k2)
            return {'ready': True, 's18a': bool(self.s18r and s18a)}
        if op == 'CMD11':
            if c.state == 'ready' and c.sig == V33 and self.s18r:
                c.state = 'sw_pending'
                return 'R1'
            return None
        if op in ('CMD2', 'CMD3', 'CMD7', 'ACMD6', 'CMD6', 'CMD19', 'CMD17'):
            if c.state not in ('ready', 'ident', 'stby', 'tran'):
                return None
            c.state = {'CMD2': 'ident', 'CMD3': 'stby', 'CMD7': 'tran'}.get(op, c.state)
            return 'ok'
        raise AssertionError(op)

    def gate(self, ms):
        self.clock_on, self.gated_ms = False, self.gated_ms + ms
        c = self.card
        if c.state == 'sw_pending' and self.gated_ms >= 1:
            c.state, self.gated_ms = 'sw_wait', ms         # the card sees the clock stop and starts the switch

    def set_bank(self, v):
        assert not self.clock_on, 'the bank changes only with the clock stopped'
        self.bank = v

    def restart(self):
        c = self.card
        if c.state == 'sw_wait':
            if self.gated_ms >= 5 and self.bank == V18:
                c.sig, c.state, c.timing = V18, 'ready', 'sdr12'      # completes: DAT goes high
            elif self.gated_ms >= 5:
                c.sig, c.state, c.timing = V18, 'ready', 'sdr12'      # the card switched; the host did not (lifetime B)
            else:
                c.state = 'sw_pending'
        self.clock_on, self.gated_ms = True, 0

    def lines_low(self):
        return self.card.state == 'sw_pending'

    def link_ok(self):
        """A read at speed: valid only when the host and card agree on the level."""
        return self.bank == self.card.sig and self.card.state in ('tran',)


def switch_steps():
    """The switch lifetime as atomic steps: (name, function(host))."""
    def acmd41(h):
        h.s18r = True
        for _ in range(3):
            r = h.cmd('ACMD41')
            if r and r['ready']:
                return r
    steps = [
        ('CMD0', lambda h: h.cmd('CMD0')),
        ('CMD8', lambda h: h.cmd('CMD8')),
        ('ACMD41 S18R', acmd41),
        ('CMD11', lambda h: h.cmd('CMD11')),
        ('clock stop', lambda h: h.gate(1)),
        ('bank to 1.8 V', lambda h: h.set_bank(V18)),
        ('clock gated 10 ms', lambda h: h.gate(10)),
        ('clock restart', lambda h: h.restart()),
        ('CMD2', lambda h: h.cmd('CMD2')),
        ('CMD3', lambda h: h.cmd('CMD3')),
        ('CMD7', lambda h: h.cmd('CMD7')),
        ('ACMD6', lambda h: h.cmd('ACMD6')),
        ('CMD6 SDR104', lambda h: (h.cmd('CMD6'), setattr(h.card, 'timing', 'sdr104'))),
        ('CMD19 sweep', lambda h: h.cmd('CMD19')),
        ('reads', lambda h: h.cmd('CMD17')),
        # the close (platform close stage CLOSE_BANK): the card clock off, then the bank back at 3.3 V; the card stays at 1.8 V
        ('close: clock off', lambda h: h.gate(10)),
        ('close: bank to 3.3 V', lambda h: h.set_bank(V33)),
    ]
    return steps


def bring_up(h, expect_level=None):
    """Identify and select the card at the current bank level (the part of a session after the level is settled)."""
    for op in ('CMD2', 'CMD3', 'CMD7', 'ACMD6'):
        if h.cmd(op) is None:
            return False
    return h.link_ok() if expect_level is None else (h.card.sig == expect_level and h.link_ok())


def complete_switch(h):
    """Gate the clock, move the bank to 1.8 V, restart: what a card holding CMD and DAT low (or already at 1.8 V) needs."""
    h.gate(10)
    h.set_bank(V18)
    h.restart()


def probe(h, level):
    h.s18r = False
    if h.cmd('CMD0') is None:
        return False
    return h.cmd('CMD8') == 'R7'


def resolve(h):
    """The session-start algorithm. Returns 'A' (usable at 3.3 V), 'C' (usable at 1.8 V) or 'UNRESOLVED'."""
    # 0. A bank left at 1.8 V is only ever the result of a completed switch by this software: go to the 1.8 V probe.
    if h.bank == V33:
        h.clock_on, h.gated_ms = False, 0
        h.restart()
        # 1. A card that holds CMD or DAT low is in the middle of the switch: finish it.
        if h.lines_low():
            complete_switch(h)
            if probe(h, V18):
                h.s18r = True
                for _ in range(3):
                    r = h.cmd('ACMD41')
                    if r and r['ready']:
                        break
                return 'C' if bring_up(h) else 'UNRESOLVED'
        # 2. The 3.3 V probe.
        if probe(h, V33):
            h.s18r = True
            for _ in range(3):
                r = h.cmd('ACMD41')
                if r and r['ready']:
                    break
            else:
                return 'UNRESOLVED'
            if r['s18a']:
                if h.cmd('CMD11') == 'R1':
                    complete_switch(h)               # the normal switch: the card was at 3.3 V
                    return 'C' if (h.card.sig == V18 and bring_up(h)) else 'UNRESOLVED'
                # S18A and no response to CMD11: the card already signals at 1.8 V (case 4 of specification 4.2.4.4)
                complete_switch(h)
                if not probe(h, V18):
                    return 'UNRESOLVED'
                h.s18r = True
                for _ in range(3):
                    r = h.cmd('ACMD41')
                    if r and r['ready']:
                        break
                return 'C' if bring_up(h) else 'UNRESOLVED'
            # S18A clear: a card that declines, or one at 1.8 V whose answer hides it. A read at speed settles it.
            if bring_up(h):
                return 'A'
            # the link does not work at 3.3 V: try the other level (the card was not at 3.3 V after all)
            complete_switch(h)
            if probe(h, V18):
                h.s18r = True
                for _ in range(3):
                    r = h.cmd('ACMD41')
                    if r and r['ready']:
                        break
                return 'C' if bring_up(h) else 'UNRESOLVED'
            return 'UNRESOLVED'
        # 3. No answer at 3.3 V: the card is at 1.8 V, absent or hung. Try 1.8 V once.
        complete_switch(h)
        if probe(h, V18):
            h.s18r = True
            for _ in range(3):
                r = h.cmd('ACMD41')
                if r and r['ready']:
                    break
            return 'C' if bring_up(h) else 'UNRESOLVED'
        h.gate(10); h.set_bank(V33); h.restart()
        return 'UNRESOLVED'
    # the bank was left at 1.8 V: gate the clock for 10 ms first (a card whose switch the cut interrupted completes it here),
    # then probe at 1.8 V; if the card does not answer, return to 3.3 V and resolve there
    h.gate(10)
    h.restart()
    if probe(h, V18):
        h.s18r = True
        for _ in range(3):
            r = h.cmd('ACMD41')
            if r and r['ready']:
                break
        return 'C' if bring_up(h) else 'UNRESOLVED'
    h.gate(10); h.set_bank(V33); h.restart()
    return resolve(h)


def rail_cycle(card, hold_ms, discharge_ms):
    """Cycle the card supply: the card is reset only if VDD stayed below 0.5 V for 1 ms, which needs the hold to exceed the discharge time."""
    if hold_ms >= discharge_ms + 1:
        card.power_cycle()
        return True
    return False


def run():
    steps = switch_steps()
    cases = unresolved_stuck = 0
    outcomes = {}
    for k1, k2, bank_resets in itertools.product((True, False), (True, False), (True, False)):
        end_bank_restored = False                       # the close is now part of the steps
        for cut in range(len(steps) + 1):
            card = Card()
            h = Host(card, k1, k2)
            for name, fn in steps[:cut]:
                if name == 'CMD11' and h.card.state != 'ready':
                    break
                fn(h)
            # session ends here: a watchdog reset or a retained fault; the next session starts fresh
            if cut == len(steps) and end_bank_restored:
                h.gate(10); h.set_bank(V33); h.restart()       # the image puts the selector back with the card still at 1.8 V
            if bank_resets or h.bank == V33:
                h.bank = V33                                   # reset value of GPIO_POW_VAL_SET_CTL
            h.clock_on, h.gated_ms, h.s18r = True, 0, False
            try:
                out = resolve(h)
            except Violation as e:
                print(f'FAIL k1={k1} k2={k2} reset={bank_resets} cut after {cut} steps: {e}')
                return 1
            cases += 1
            outcomes[out] = outcomes.get(out, 0) + 1
            if out == 'UNRESOLVED':
                print(f'FAIL k1={k1} k2={k2} bank_resets={bank_resets} restored={end_bank_restored} cut after {cut} steps '
                      f'({steps[cut - 1][0] if cut else "none"}): card {card.sig} {card.state}, bank {h.bank}')
                return 1
            assert card.sig == h.bank, (out, card.sig, h.bank)
    # a card without UHS-I support declines the switch and stays usable at 3.3 V
    for k1, k2 in itertools.product((True, False), (True, False)):
        card = Card(uhs=False)
        h = Host(card, k1, k2)
        assert resolve(h) == 'A' and card.sig == V33 and h.bank == V33
    # a card that does not answer at 1.8 V is not recoverable by the algorithm; the card supply cycle recovers it
    for k1, k2 in itertools.product((True, False), (True, False)):
        card = Card(stuck=True)
        h = Host(card, k1, k2)
        for name, fn in steps[:8]:
            fn(h)
        h.bank = V33; h.clock_on, h.gated_ms, h.s18r = True, 0, False
        out = resolve(h)
        assert out == 'UNRESOLVED' and card.sig == V18, (k1, k2, out)
        unresolved_stuck += 1
        for hold, discharge in ((50, 400), (200, 400), (500, 400)):
            reset = rail_cycle(card, hold, discharge)
            assert reset == (hold > discharge), (hold, discharge)
            if reset:
                break
        assert card.sig == V33 and card.state == 'idle'
        h = Host(card, k1, k2)
        assert resolve(h) == 'C' and card.sig == V18       # a fresh 3.3 V card takes the normal switch
    print(f'PASS sd-uhs recovery model: {cases} sessions cut at every step of the switch lifetime x K1 x K2 x reset behaviour (the close, with the bank back at 3.3 V, is the last two steps), '
          f'outcomes {outcomes}, no command with the bank at 1.8 V and the card at 3.3 V; {unresolved_stuck} stuck-card cases are '
          f'unresolved by software and cleared by the supply cycle once the hold exceeds the discharge time')
    return 0


if __name__ == '__main__':
    sys.exit(run())
