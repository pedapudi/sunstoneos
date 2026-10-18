#ifndef SUNSTONE_ADC_SNAPSHOT_H
#define SUNSTONE_ADC_SNAPSHOT_H
#include "adc_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int adc_session_prepare(const struct a733_smp_contract *);
const struct adc_session *adc_admitted_session(void);
#endif
