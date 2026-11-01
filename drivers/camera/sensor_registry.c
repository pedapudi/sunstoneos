/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "camera_sensor.h"
#include <string.h>
/* Driver registration data is independent of class lifecycle and board wiring. */
const struct camera_sensor_driver *const camera_sensor_drivers[]={&camera_sensor_ov5640,&camera_sensor_ov5647};
const size_t camera_sensor_driver_count=sizeof(camera_sensor_drivers)/sizeof(camera_sensor_drivers[0]);
const struct camera_sensor_driver *camera_sensor_find(const char *name)
{
    if(!name)return NULL;
    for(size_t i=0;i<camera_sensor_driver_count;i++)if(!strcmp(name,camera_sensor_drivers[i]->name))return camera_sensor_drivers[i];
    return NULL;
}
