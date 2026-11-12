/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_BOARD_H
#define SUNSTONE_BOARD_H
#include <stddef.h>
#include <stdint.h>
#include "sunstone_cpu_topology.h"
struct camera_sensor_bus;
struct sunstone_display_port;
/* Wiring and module selection describe a board configuration. They do not
 * establish attachment, electrical compatibility or session authority. */
struct sunstone_usb_port {
    const char *name;
    uint8_t controller, typec_i2c_controller, typec_address;
    uint8_t vbus_bank, vbus_pin, vbus_active_high;
    uint8_t irq_bank, irq_pin, scl_bank, scl_pin, sda_bank, sda_pin, i2c_function;
    uint8_t supplies[2];
    uint32_t supply_uv;
};
/* A two-wire controller routed to the 40-pin header. bus is an enum a733_i2c_bus_id
 * value (include/a733_i2c_bus.h). The header numbers are the physical positions in
 * the board manual V1.0 section 3.16.4 and the V1.2 schematic sheet 17. */
struct sunstone_i2c_route {
    const char *name;
    uint8_t bus, bank, scl_pin, sda_pin, function, header_scl, header_sda;
};
struct sunstone_board_description {
    const char *name;
    const struct ss_cpu_policy *cpu_policy;
    const struct camera_sensor_bus *camera;
    size_t camera_count;
    const char *const *camera_sensors;
    size_t camera_sensor_count;
    const struct sunstone_display_port *display_ports;
    size_t display_port_count;
    const struct sunstone_usb_port *usb_ports;
    size_t usb_port_count;
    const struct sunstone_i2c_route *i2c_routes;
    size_t i2c_route_count;
};
extern const struct ss_cpu_policy sunstone_orangepi_zero3w_cpu_policy;
const struct sunstone_board_description *sunstone_board_orangepi_zero3w(void);
#endif
