#ifndef SUNSTONE_FREERTOS_IP_CONFIG_H
#define SUNSTONE_FREERTOS_IP_CONFIG_H
#define FREERTOS_IP_CONFIG_H
#include <stdint.h>
void sunstone_net_stack_free(void *);
#define vPortFreeSocket(pointer) sunstone_net_stack_free(pointer)
#define ipconfigBYTE_ORDER pdFREERTOS_LITTLE_ENDIAN
#define ipconfigIP_TASK_PRIORITY 3
#define ipconfigIP_TASK_STACK_SIZE_WORDS 4096
#define ipconfigEVENT_QUEUE_LENGTH 32
#define ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS 24
#define ipconfigNETWORK_MTU 1500
#define ipconfigUSE_IPv4 1
#define ipconfigUSE_IPv6 0
#define ipconfigIPv4_BACKWARD_COMPATIBLE 0
#define ipconfigUSE_DHCP 0
#define ipconfigUSE_DHCPv6 0
#define ipconfigUSE_RA 0
#define ipconfigUSE_DNS 0
#define ipconfigUSE_LLMNR 0
#define ipconfigUSE_MDNS 0
#define ipconfigUSE_NBNS 0
#define ipconfigUSE_TCP 1
#define ipconfigUSE_TCP_WIN 1
#define ipconfigTCP_RX_BUFFER_LENGTH (4 * 1460)
#define ipconfigTCP_TX_BUFFER_LENGTH (4 * 1460)
#define ipconfigTCP_WIN_SEG_COUNT 32
#define ipconfigTCP_MSS 1460
#define ipconfigUDP_MAX_RX_PACKETS 4
#define ipconfigUDP_MAX_SEND_BLOCK_TIME_TICKS 0
#define ipconfigSOCK_DEFAULT_RECEIVE_BLOCK_TIME 0
#define ipconfigSOCK_DEFAULT_SEND_BLOCK_TIME 0
#define ipconfigSUPPORT_SELECT_FUNCTION 0
#define ipconfigSUPPORT_SIGNALS 0
#define ipconfigUSE_CALLBACKS 0
#define ipconfigSOCKET_HAS_USER_SEMAPHORE 0
#define ipconfigSOCKET_HAS_USER_WAKE_CALLBACK 0
#define ipconfigUSE_NETWORK_EVENT_HOOK 0
#define ipconfigUSE_LINKED_RX_MESSAGES 0
#define ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM 0
#define ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM 0
#define ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES 0
#define ipconfigETHERNET_DRIVER_FILTERS_PACKETS 0
#define ipconfigZERO_COPY_RX_DRIVER 0
#define ipconfigZERO_COPY_TX_DRIVER 0
#define ipconfigARP_CACHE_ENTRIES 8
#define ipconfigFILTER_OUT_NON_ETHERNET_II_FRAMES 1
#define ipconfigUSE_OUTGOING_PINGS 0
#define ipconfigREPLY_TO_INCOMING_PINGS 1
#define ipconfigHAS_PRINTF 0
#define ipconfigHAS_DEBUG_PRINTF 0
#define ipconfigTCP_KEEP_ALIVE 0
#define ipconfigTCP_HANG_PROTECTION 1
#define ipconfigALLOW_SOCKET_SEND_WITHOUT_BIND 0
#define ipconfigDNS_CHECK_REPLY_SOURCE_IP 1
#endif
