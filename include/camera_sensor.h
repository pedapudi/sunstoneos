/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_CAMERA_SENSOR_H
#define SUNSTONE_CAMERA_SENSOR_H
#include <stddef.h>
#include <stdint.h>

enum camera_pixel_format { CAMERA_UYVY, CAMERA_YUYV, CAMERA_SBGGR8, CAMERA_SBGGR10 };
struct camera_mode {
    uint32_t xclk_hz,link_hz,pixel_hz;
    uint16_t width,height,htotal,vtotal;
    uint8_t lanes,virtual_channel,csi_data_type,bytes_per_pixel;
    uint8_t size,format; /* Driver mode index; enum camera_pixel_format. */
};
enum camera_sensor_line { CAMERA_SENSOR_PWDN_LINE, CAMERA_SENSOR_RESET_LINE };
struct camera_pin { uint8_t bank,pin,function; };
struct camera_gpio { uint8_t bank,pin,active_high,present; };
/* Electrical wiring belongs to the board/module description. Regulator IDs
 * name existing platform supplies; this class never programs their voltage.
 * acquire establishes stable rails/XCLK, PWDN asserted and RESET asserted if
 * present. power/reset/bus holds are minimum microseconds after each effect.
 * i2c_address is the one admitted module address: no bus-wide address scan. */
struct camera_sensor_bus {
    uint8_t port,parser,lanes,virtual_channel;
    uint8_t i2c_controller,i2c_address,mclk;
    uint32_t xclk_hz;
    struct camera_gpio pwdn,reset;
    uint16_t regulator[3];
    uint32_t power_hold_us,reset_hold_us,bus_hold_us;
    struct camera_pin csi[10],mclk_pin,i2c_scl,i2c_sda;
};
/* One privileged serialized owner; fresh zero storage and immutable private
 * inputs. Tokens remain held after uncertainty. No reentry, allocation, retry
 * or rollback. read/write/line/block require exactly 1; zero faults. acquire,
 * receiver and release may return 0 while pending. Every callback is bounded
 * by its absolute microsecond deadline. now_us must remain monotonic. */
struct camera_sensor_bus_ops {
    int (*now_us)(void *,uint64_t *);
    int (*acquire)(void *,uint64_t,uint64_t *);
    int (*line)(void *,uint64_t,enum camera_sensor_line,int,uint64_t);
    int (*read)(void *,uint64_t,uint8_t,uint16_t,uint8_t *,uint64_t);
    int (*write)(void *,uint64_t,uint8_t,uint16_t,uint8_t,uint64_t);
    int (*receiver)(void *,uint64_t,uint64_t,const struct camera_mode *,uint64_t);
    int (*release)(void *,uint64_t,uint64_t);
    int (*write_block)(void *,uint64_t,uint8_t,uint16_t,const uint8_t *,unsigned,uint64_t);
    /* Called immediately before the first write that can emit packets.
     * It arms the DMA status interval before bus latency. Failure retains all
     * resources and prevents that write. The adapter owns register knowledge. */
    int (*before_stream)(void *,uint64_t);
};
#define CAMERA_SENSOR_BLOCK_BYTES 40u
#define CAMERA_SENSOR_PRIVATE_BYTES 512u
enum camera_sensor_state {
    CAMERA_SENSOR_NEW,CAMERA_SENSOR_ACQUIRE,CAMERA_SENSOR_POWER_WAIT,CAMERA_SENSOR_PWDN,
    CAMERA_SENSOR_RESET_WAIT,CAMERA_SENSOR_RESET,CAMERA_SENSOR_BUS_WAIT,CAMERA_SENSOR_IDENTIFY,
    CAMERA_SENSOR_CONFIGURE,CAMERA_SENSOR_READY,CAMERA_SENSOR_RECEIVER,CAMERA_SENSOR_STARTING,
    CAMERA_SENSOR_STREAMING,CAMERA_SENSOR_STOPPING,CAMERA_SENSOR_RELEASING,
    CAMERA_SENSOR_CLOSED,CAMERA_SENSOR_FAULT
};
enum camera_sensor_control { CAMERA_SENSOR_TEST_PATTERN, CAMERA_SENSOR_EXPOSURE,
    CAMERA_SENSOR_GAIN, CAMERA_SENSOR_WHITE_BALANCE };
/* The current adapters support test-pattern selection before power-on.
 * Exposure, gain and white-balance IDs are reserved but return unsupported. */
struct camera_sensor_control_info { int32_t minimum,maximum,step,default_value; uint8_t before_open; };
struct camera_sensor_driver;
int camera_sensor_control_query(const struct camera_sensor_driver *,enum camera_sensor_control,
    struct camera_sensor_control_info *);
struct camera_sensor;
struct camera_sensor_driver {
    const char *name;
    uint8_t address;
    uint16_t id_register[2];
    uint8_t id_value[2];
    uint32_t power_hold_us,reset_hold_us,bus_hold_us;
    uint16_t mode_count;
    int (*enumerate)(unsigned,unsigned,unsigned,struct camera_mode *);
    /* Bind the already powered and identified module. No I/O here. */
    int (*bind)(struct camera_sensor *);
    int (*start)(struct camera_sensor *,uint64_t,uint64_t);
    int (*stop)(struct camera_sensor *,uint64_t);
    int (*close)(struct camera_sensor *,uint64_t);
    int (*poll)(struct camera_sensor *);
    int (*control)(struct camera_sensor *,enum camera_sensor_control,int);
    int (*control_query)(enum camera_sensor_control,struct camera_sensor_control_info *);
};
struct camera_sensor {
    struct camera_sensor_bus_ops ops;
    void *context;
    struct camera_sensor_bus bus;
    const struct camera_sensor_driver *const *drivers;
    size_t driver_count,candidate;
    const struct camera_sensor_driver *driver;
    struct camera_mode mode;
    uint64_t token,deadline,now,wait_until;
    enum camera_sensor_state state;
    uint16_t index;
    uint8_t initialized,test_pattern;
    union { uint64_t align; uint8_t bytes[CAMERA_SENSOR_PRIVATE_BYTES]; } private;
};
extern const struct camera_sensor_driver camera_sensor_ov5640,camera_sensor_ov5647;
extern const struct camera_sensor_driver *const camera_sensor_drivers[];
extern const size_t camera_sensor_driver_count;
const struct camera_sensor_driver *camera_sensor_find(const char *);
int camera_sensor_mode(const struct camera_sensor_driver *,unsigned,unsigned,unsigned,struct camera_mode *);
int camera_sensor_init(struct camera_sensor *,const struct camera_sensor_bus_ops *,void *,
    const struct camera_sensor_bus *,const struct camera_sensor_driver *const *,size_t,
    const struct camera_mode *,int);
int camera_sensor_open(struct camera_sensor *,uint64_t);
int camera_sensor_start(struct camera_sensor *,uint64_t,uint64_t);
int camera_sensor_stop(struct camera_sensor *,uint64_t);
int camera_sensor_close(struct camera_sensor *,uint64_t);
int camera_sensor_control_set(struct camera_sensor *,enum camera_sensor_control,int);
int camera_sensor_poll(struct camera_sensor *);
/* Callback helpers retain the first failure and enforce pre/post deadlines. */
int camera_sensor_fault(struct camera_sensor *);
int camera_sensor_time(struct camera_sensor *);
#endif
