#ifndef SUNSTONE_USB0_STOP_H
#define SUNSTONE_USB0_STOP_H
/* Stable USB0 guard identifiers. The OS stores the identifier in the USB0
 * failure site's low 12 bits, provider step/rail index in its 15-bit aux,
 * and the signed callback result in the value word. */
#define USB0_STOP_PREPARE 1u
#define USB0_STOP_POLL_ENABLE 2u
#define USB0_STOP_TRACE 3u
#define USB0_STOP_ASSERT 4u
#define USB0_STOP_DELAY 5u
#define USB0_STOP_START_LIVE 6u
#define USB0_STOP_START_PROVIDER 7u
#define USB0_STOP_STACK_INIT 8u
#define USB0_STOP_PASS_LIVE 9u
#define USB0_STOP_RX_INTERFACE 10u
#define USB0_STOP_TX_INTERFACE 11u
#define USB0_STOP_TX_TRANSFER 12u
#define USB0_STOP_RESULT_REQUEST 13u
#define USB0_STOP_FAULT_REQUEST 14u
#define USB0_STOP_NPU_REQUEST 15u
#define USB0_STOP_THERMAL_REQUEST 16u
#define USB0_STOP_PERF_REQUEST 17u
#define USB0_STOP_OUTPUT_LIVE 18u
#define USB0_STOP_OUTPUT_EXPORT 19u
#define USB0_STOP_REPLY_EXPORT 20u
#define USB0_STOP_RECEIVE 21u
#define USB0_STOP_OFFER 22u
#define USB0_STOP_PROVIDER_RETAIN 23u
#define USB0_STOP_RETURN_ACTIVE 24u
#define USB0_STOP_RETURN_PROOF 25u
#define USB0_STOP_REGISTRATION 26u
#endif
