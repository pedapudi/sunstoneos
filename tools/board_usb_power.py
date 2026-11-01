"""Observe and recover the Zero 3W TYPEC24 VBUS enable on PL8."""

CFG, DAT, PULL = 0x07025004, 0x07025010, 0x07025024
DATA_MASK = 1 << 8


class PowerFault(ValueError):
    pass


def snapshot(tx):
    raw = tx(*[part for address in (CFG, DAT, PULL) for part in ('readl', hex(address))])
    values = [int(word, 16) for word in raw.split()]
    if len(values) != 3 or any(not 0 <= word <= 0xffffffff for word in values):
        raise PowerFault('PL8 register snapshot is malformed; source state is unknown.')
    cfg, dat, pull = values
    mux, level, bias = cfg & 15, (dat >> 8) & 1, (pull >> 16) & 3
    # Input/disabled rely on the fitted R45 pull-down. An internal pull-up
    # or reserved pull encoding cannot establish that the source is off.
    safe = (mux == 1 and level == 0 and bias != 3) or (mux in (0, 15) and bias in (0, 2))
    return dict(cfg=cfg, dat=dat, pull=pull, mux=mux, data=level,
                bias=bias, source_off=safe, output_high=mux == 1 and level == 1)


def observe(owner, phase):
    event = {'phase': phase, 'identity': list(owner.identity)}
    owner.record.setdefault('usb_vbus', []).append(event)
    try:
        event['before'] = before = snapshot(owner.tx)
        owner.journal()
        if before['output_high']:
            owner.record.setdefault('usb_vbus_fault', {'site': 'usb-vbus-output-high',
                'detail': 'PL8 remained output-high at '+phase, 'inputs': before})
            owner.journal()
            # DAT is written first while PL8 is already an output. Preserve
            # every other pin; neither the mux nor pull register is changed.
            owner.tx('writel', hex(DAT), hex(before['dat'] & ~DATA_MASK))
            event['after'] = after = snapshot(owner.tx)
            if (after['cfg'] != before['cfg'] or after['pull'] != before['pull'] or
                    after['dat'] != before['dat'] & ~DATA_MASK or not after['source_off']):
                raise PowerFault('PL8 low recovery readback differs; source state is unresolved.')
            event['recovered_low'] = True
        elif not before['source_off']:
            owner.record.setdefault('usb_vbus_fault', {'site': 'usb-vbus-source-unknown',
                'detail': 'PL8 source state is unknown at '+phase, 'inputs': before})
            raise PowerFault('PL8 mux/pull does not establish source-off at '+phase)
    except (ValueError, PowerFault) as error:
        event['failure'] = str(error)
        owner.quarantine(str(error))
        raise PowerFault(str(error)) from error
    finally:
        owner.journal()


def enforce(owner):
    fault = owner.record.get('usb_vbus_fault')
    if fault:
        raise PowerFault(fault['detail'])
    if owner.record.get('usb_vbus_cleanup_failure'):
        raise PowerFault(owner.record['usb_vbus_cleanup_failure'])


def admit_diagnostic(owner):
    """Capture the clocks used by the diagnostic's private S_TWI1 adapter."""
    addresses = (0x07010010, 0x07090160, 0x0701019c)
    values = [int(word, 16) for word in owner.tx(*[
        part for address in addresses for part in ('readl', hex(address))]).split()]
    if len(values) != 3:
        raise PowerFault('VBUS diagnostic clock snapshot is malformed.')
    owner.record['usb_vbus_clock_admission'] = dict(zip(('apbs1', 'xo_ctrl0', 's_twi_bgr'), values))
    owner.journal()
    apbs1, oscillator, bgr = values
    if apbs1 & 0x0700001f or oscillator & 0x10000002 != 0x10000002 or bgr & 0x00020002:
        raise PowerFault('VBUS diagnostic requires raw DCXO/div1 APBS1, enabled DCXO and idle S_TWI1.')
