#!/usr/bin/env python3
"""Decode a copied PWM register/IRQ/stop record; no physical waveform claim."""
import hashlib,struct
def decode(raw,session):
 if len(session)!=784 or struct.unpack_from('<3Q',session,592)!=(0x4153373350574131,1,784):raise ValueError('Wrong PWM session')
 if len(raw)!=256:raise ValueError('Incomplete PWM record')
 magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
 if (magic,version)!=(0x314d5750,1) or status not in (0,1,2) or count!=256:raise ValueError('Wrong PWM framing')
 if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=session[48:80] or raw[64:96]!=hashlib.sha256(session).digest():raise ValueError('PWM session/build differs')
 names=('phase','error','started','irq_seen','pisr','pcr','ppr','counter','pulse_counter','waveform_stopped','gpio_low','bgr','apb0','voltage_mode','voltage_value','reserved')
 report=dict(zip(names,struct.unpack_from('<16I',raw,96)))
 started,completed=struct.unpack_from('<2Q',raw,160);plan=struct.unpack_from('<4IQ',raw,176)
 rail=struct.unpack_from('<I',raw,200)[0];enabled,decoded,uv=struct.unpack_from('<3I',raw,212)
 deadline=struct.unpack_from('<Q',raw,224)[0];initialized,fault,clock_retained,rail_retained,app_error,reserved=struct.unpack_from('<6I',raw,232)
 if report['reserved'] or reserved or any(raw[210:212]):raise ValueError('Nonzero PWM reserved field')
 if status==1:
  if report['phase']!=16 or report['error'] or not all(report[k]==1 for k in ('started','irq_seen','waveform_stopped','gpio_low')):raise ValueError('PWM completion/stop facts missing')
  if plan!=(0x0f9f0300,0x0257012c,600,300,100000) or report['ppr']!=plan[1] or report['pcr']!=plan[0]:raise ValueError('Different PWM profile')
  if rail!=15 or raw[204]!=0 or raw[205]!=3 or raw[206]!=0 or raw[209]!=0 or (enabled,decoded,uv)!=(1,1,1800000):raise ValueError('PWM supply differs')
  if initialized!=1 or fault or app_error or not 0<started<=completed<deadline:raise ValueError('PWM lifetime/state differs')
 return dict(status=status,epoch=epoch,generation=generation,report=report,started_us=started,completed_us=completed,
  plan=dict(pcr=plan[0],ppr=plan[1],ticks=plan[2],active=plan[3],duration_us=plan[4]),
  supply=dict(rail=rail,page_before=raw[204],chip=raw[205],revision=raw[206],enable=raw[207],selector=raw[208],page_after=raw[209],enabled=enabled,decoded=decoded,programmed_uv=uv),
  deadline_us=deadline,initialized=initialized,fault=fault,clock_retained=clock_retained,rail_retained=rail_retained,application_error=app_error,
  physical_waveform='not established by register evidence',physical_fan_off='requires gate/current measurement')
