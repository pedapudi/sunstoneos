/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_USB2_NATIVE_H
#define SUNSTONE_A733_USB2_NATIVE_H
#include "a733_usb2_platform.h"
#include "a733_xhci.h"
struct a733_platform_locks;
#define A733_USB2_HID_REPORT_BYTES 64u
#define A733_USB2_HUBS 2u /* Hub states: the root-port hub and one hub below it. */
#define A733_USB2_AUDIO_ALTERNATES 4u
#define A733_USB2_AUDIO_BLOCK_BYTES 1024u
enum a733_usb2_device_kind { A733_USB2_DEVICE_NONE,A733_USB2_DEVICE_MSC,A733_USB2_DEVICE_HID,A733_USB2_DEVICE_HUB,
    A733_USB2_DEVICE_DUAL,A733_USB2_DEVICE_AUDIO };
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
/* record is the hub's xHCI device record index plus one. A bus-powered hub
 * draws its configuration current plus one unit load (100 mA) per port. */
struct a733_usb2_hub {
    unsigned present,record,bus_powered,max_power_ma;
    unsigned address,protocol,endpoint,packet,interval,ports,ttt,power_good_2ms;
    unsigned mark_pending,mark_address,configure_pending;
    unsigned device_power,local_power,alt_selected,powered_mask,selected_port,connected_mask;
    unsigned status_deferred,status_requested,status_bytes;
    unsigned depth_set,hdr_latency,delay_ns,u1_disabled,u2_disabled;
    uint64_t power_good_deadline;
    uint8_t *status_buffer;
};
/* Copied PCM capture, not a decoded or resampled stream. Frames are whole
 * interleaved S16_LE samples in the order received. dropped counts service
 * intervals that delivered no data, overruns counts empty-ring reports and
 * overwritten counts frames replaced in the class FIFO before a read; all
 * three are totals since the stream started. */
struct a733_usb2_audio_block {
    uint64_t generation,sequence;
    uint32_t rate,frames,dropped,overruns,overwritten;
    uint8_t address,interface,endpoint,speed,channels,sample_bytes,reserved[2];
    uint8_t data[A733_USB2_AUDIO_BLOCK_BYTES];
};
/* One admitted USB Audio Class 1 capture alternate setting. */
struct a733_usb2_audio_alternate { unsigned interface,alternate,packet,interval; };
struct a733_usb2_audio {
    unsigned address,control_interface,streaming,endpoint,alternates;
    uint32_t features[8]; /* Feature Unit IDs of the Audio Control interface, one bit each. */
    struct a733_usb2_audio_alternate alternate[A733_USB2_AUDIO_ALTERNATES];
    unsigned stream,interface,packet,channels,starting,started;
    uint32_t rate;
    uint64_t start_deadline,sequence,written,read;
    uint8_t *pending_buffer;
    unsigned pending_bytes;
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
    struct a733_usb2_hub hubs[A733_USB2_HUBS];
    struct a733_usb2_audio audio;
    struct a733_usb2_descriptor descriptors[A733_XHCI_DEVICES];
    /* Admission options set before the first poll. hub_tiers 2 admits a USB2
     * high-speed hub below the root-port hub; bus_powered_hubs admits USB2 hubs
     * without local power within the upstream current budget; audio_capture
     * admits one USB Audio Class 1 capture device in images built with the
     * TinyUSB audio class. */
    unsigned hub_tiers,bus_powered_hubs,audio_capture;
    unsigned dual_leaf,msc_seen,hid_seen,audio_seen,msc_mounted,hid_mounted,audio_mounted,hid_address,msc_in,msc_out;
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
/* Select the capture configuration with S16_LE samples at rate and channels,
 * open its isochronous endpoint and start the stream; completion must follow
 * by deadline. Returns1 when started, 0 when no such configuration exists. */
int a733_usb2_audio_start(struct a733_usb2_native *,uint32_t rate,unsigned channels,uint64_t deadline);
/* Copy the captured frames available now, at most one block, after a fresh
 * live attachment observation. Returns1 with frames, 0 with none, -1 failed. */
int a733_usb2_audio_read(struct a733_usb2_native *,struct a733_usb2_audio_block *);
void a733_usb2_native_lost(struct a733_usb2_native *);
/* Internal TinyUSB composition; singleton static stack, one task and one host.
 * These entry points also permit the actual HCD/MSC stack in the host fixture. */
int a733_usb2_host_bind(struct a733_usb2_native *);
int a733_usb2_host_poll(struct a733_usb2_native *);
void a733_usb2_host_notice(void *,const struct a733_xhci_notice *);
#endif
