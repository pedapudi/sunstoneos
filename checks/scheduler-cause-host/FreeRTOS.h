#ifndef SS_CAUSE_HOST_FREERTOS_H
#define SS_CAUSE_HOST_FREERTOS_H
#define configNUMBER_OF_CORES 8
#define configUNIQUE_INTERRUPT_PRIORITIES 16
#define portSGI1R_VALUE(interrupt,core) (((unsigned long long)(core)<<16)|1)
#endif
