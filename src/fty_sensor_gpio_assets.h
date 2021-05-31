/*  =========================================================================
    fty_sensor_gpio_assets - 42ITy GPIO assets handler

    Copyright (C) 2014 - 2020 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

#pragma once
#include <malamute.h>

///  Structure of our class
struct fty_sensor_gpio_assets_t
{
    char*         name;         // actor name
    mlm_client_t* mlm;          // malamute client
    zlistx_t*     gpx_list;     // List of monitored GPx _gpx_info_t (10xGPI / 5xGPO on IPC3000)
    char*         template_dir; // Location of the template files
    bool          test_mode;    // true if we are in test mode, false otherwise
};

///  fty_sensor_gpio_assets actor
void fty_sensor_gpio_assets(zsock_t* pipe, void* args);

///  Create a new fty_sensor_gpio_assets
fty_sensor_gpio_assets_t* fty_sensor_gpio_assets_new(const char* name);

///  Destroy the fty_sensor_gpio_assets
void fty_sensor_gpio_assets_destroy(fty_sensor_gpio_assets_t** self_p);

///  Self test of this class
void fty_sensor_gpio_assets_test(bool verbose);

///  List accessor
int add_sensor(fty_sensor_gpio_assets_t* self, const char* operation, const char* manufacturer, const char* assetname,
    const char* extname, const char* asset_subtype, const char* sensor_type, const char* sensor_normal_state,
    const char* sensor_gpx_number, const char* sensor_gpx_direction, const char* sensor_parent,
    const char* sensor_location, const char* sensor_power_source, const char* sensor_alarm_message,
    const char* sensor_alarm_severity);

void request_sensor_power_source(fty_sensor_gpio_assets_t* self, const char* asset_name);
