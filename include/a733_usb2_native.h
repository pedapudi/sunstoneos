/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_USB2_NATIVE_H
#define SUNSTONE_A733_USB2_NATIVE_H
#include "a733_usb2_platform.h"
#include "a733_xhci.h"
struct a733_platform_locks;
#define A733_USB2_HID_REPORT_BYTES 64u
enum a733_usb2_device_kind { A733_USB2_DEVICE_NONE,A733_USB2_DEVICE_MSC,A733_USB2_DEVICE_HID,A733_USB2_DEVICE_HUB,A733_USB2_DEVICE_DUAL };
/* Raw completed interrupt input, not a desktop keyboard/mouse event. Zero or
 * short reports retain their actual lengths; no boot-report semantics inferred. */
struct a733_usb2_hid_report {
    uint64_t generation,sequence;
    uint32_t bytes;
    uint8_t address,interface,protocol,endpoint,speed,reserved[7];
    uint8_t data[A733_USB2_HID_REPORT_BYTES];
};
struct a733_usb2_hid {
    unsigned endpoint,packet,interval,subclass,protocol,descriptor_bytes;
    unsigned pending,complete,event_seen,event_bytes,requested;
    uint64_t deadline,sequence;
    uint8_t *pending_buffer;
    uint8_t descriptor[256],copied[A733_USB2_HID_REPORT_BYTES];
};
struct a733_usb2_ss {
    uint8_t ss_bos_prefix[5],ss_sel[6];unsigned ss_bos_bytes,ss_bos_seen,ss_ready,ss_control,ss_request;
    unsigned ss_u1,ss_u2,ss_burst[2];
    uint8_t ss_bos[256];
};
struct a733_usb2_descriptor {
    struct a733_usb2_ss ss;
    unsigned kind,device_probe,configuration_probe,string_bytes,string_id,string_language;
    uint8_t device_prefix[8],configuration_prefix[9];
};
struct a733_usb2_hub {
    unsigned present,address,protocol,endpoint,packet,interval,ports,ttt,power_good_2ms;
    unsigned mark_pending,mark_address,configure_pending;
    unsigned device_power,local_power,alt_selected,powered_mask,selected_port,connected_mask;
    unsigned status_deferred,status_requested,status_bytes;
    unsigned depth_set,hdr_latency,delay_ns,u1_disabled,u2_disabled;
    uint64_t power_good_deadline;
    uint8_t *status_buffer;
};
struct a733_usb2_native {
    struct a733_usb2_native *self;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_context;
    int (*admit)(void *,const struct a733_usb2_platform_config *);
    struct a733_usb2_platform platform;
    struct a733_usb2_platform_ops platform_ops;
    struct a733_xhci core;
    struct a733_xhci_ops core_ops;
    struct device_memory_pool *parent_pool,private_pool;
    struct device_memory_lease parent,memory;
    struct device_memory_extent backing;
    uint64_t last_now,read_deadline;
    /* Bound for mounting while unmounted: the startup deadline, then the same
     * budget from each attachment after a removal; zero while none waits. */
    uint64_t mount_deadline,mount_budget;
    enum a733_usb2_device_kind kind;
    struct a733_usb2_hid hid;
    struct a733_usb2_hub hub;
    struct a733_usb2_descriptor descriptors[A733_XHCI_DEVICES];
    unsigned dual_leaf,msc_seen,hid_seen,msc_mounted,hid_mounted,hid_address,msc_in,msc_out;
    unsigned initialized,core_started,stack_started,failed,address,mounted,
        read_pending,read_complete,read_failed;
    uint32_t blocks,block_bytes,bot_tag,bot_total,bot_received;
    unsigned bot_phase,bot_opcode,bot_stage,bot_endpoint,bot_requested,bot_status;
    uint8_t *bot_buffer,*control_buffer;
    unsigned control_bytes,control_address,control_device;
    uint8_t control_setup[8];
    _Alignas(64) uint8_t sector[512];
};
/* Privileged boot constructor, EL1 before the scheduler. Reserve a permanent
 * NC parent through the actual shared allocator; initialize a private child
 * ledger within it. All mappings are identity and immutable; no stage2, alias,
 * remote master or other software owner may access the reservation. Parent
 * allocation/transition occurs before concurrency. Failure retains it.
 * Native admits only Normal NC DMA and Device MMIO, including complete xHCI,
 * DWC3, PHY and exact shared-register pages. The actual authority callback
 * validates board/rail/recovery/remote-exclusion evidence, not a ready flag.
 */
int a733_usb2_native_init(struct a733_usb2_native *,const struct a733_usb2_platform_config *,
    struct a733_platform_locks *,void *owner_task,struct device_memory_pool *,
    struct a733_clock_leases *,struct a733_rail_leases *,struct a733_pmic *,
    struct a733_gpu_resource_manager *,struct a733_typec *,
    int (*admit)(void *,const struct a733_usb2_platform_config *),void *);
/* Designated task, finite lifetime. One core event/state and a bounded TinyUSB
 * queue drain per poll. Type-C power shutdown runs on independent tasks.
 * READY means MSC enumeration/capacity or HID configuration completed; no data result is implied.
 */
enum a733_usb2_result a733_usb2_native_poll(struct a733_usb2_native *);
/* Read one logical512-byte sector from LUN0. Driver-owned backing persists;
 * no caller address reaches DMA. Result copies only after successful READ10
 * data plus command status and a fresh live attachment observation. */
int a733_usb2_read_sector(struct a733_usb2_native *,uint32_t lba,uint64_t deadline);
int a733_usb2_read_result(struct a733_usb2_native *,uint8_t out[512]);
/* Same actual owner task. One interrupt IN request at a time; caller storage
 * never reaches DMA. Result copies the exact completed raw bytes only after
 * a fresh live attachment/deadline check. No automatic rearm or report parser.
 * Removing a device ends its outstanding request without output: results
 * return-1 and the kind is NONE until a later attachment mounts. */
enum a733_usb2_device_kind a733_usb2_device_kind(struct a733_usb2_native *);
int a733_usb2_hid_receive(struct a733_usb2_native *,uint64_t deadline);
int a733_usb2_hid_result(struct a733_usb2_native *,struct a733_usb2_hid_report *);
void a733_usb2_native_lost(struct a733_usb2_native *);
/* Internal TinyUSB composition; singleton static stack, one task and one host.
 * These entry points also permit the actual HCD/MSC stack in the host fixture. */
int a733_usb2_host_bind(struct a733_usb2_native *);
int a733_usb2_host_poll(struct a733_usb2_native *);
void a733_usb2_host_notice(void *,const struct a733_xhci_notice *);
#endif
