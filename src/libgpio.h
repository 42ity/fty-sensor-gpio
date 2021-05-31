/*  =========================================================================
    libgpio - General Purpose Input/Output (GPIO) sensors library

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
#include <czmq.h>
#include <string>

// Default target address of the GPIO chipset (gpiochip488 on IPC3000)
#define GPIO_BASE_INDEX 488

// Directions
#define GPIO_DIRECTION_IN  0
#define GPIO_DIRECTION_OUT 1

// GPIO Status value
#define GPIO_STATE_UNKNOWN -1
#define GPIO_STATE_CLOSED  0
#define GPIO_STATE_OPENED  1

// Defines
#define GPIO_BUFFER_MAX    4
#define GPIO_DIRECTION_MAX 64 // 35
#define GPIO_VALUE_MAX     64 // 30
#define GPIO_MAX_RETRY     3

#define GPIO_POWERED_SELF     1
#define GPIO_POWERED_EXTERNAL 2

///  Structure of our class
struct libgpio_t
{
    int       gpio_base_address; // Base address of the GPIOs chipset
    bool      test_mode;         // true if we are in test mode, false otherwise
    int       gpo_offset;        // offset to access GPO pins
    int       gpi_offset;        // offset to access GPI pins
    int       gpo_count;         // number of supported GPO
    int       gpi_count;         // number of supported GPI
    zhashx_t* gpi_mapping;       // mapping for GPIs
    zhashx_t* gpo_mapping;       // mapping for GPOs
};

///  Create a new libgpio
libgpio_t* libgpio_new(void);

///  Compute and store HW pin number
int libgpio_compute_pin_number(libgpio_t* self, int GPx_number, int direction);

///  Read a GPI or GPO status
int libgpio_read(libgpio_t* self_p, int GPx_number, int direction = GPIO_DIRECTION_IN);

///  Write a GPO (to enable or disable it)
int libgpio_write(libgpio_t* self_p, int GPO_number, int value);

///  Get the textual name for a status
std::string libgpio_get_status_string(int value);

///  Get the numeric value for a status name
int libgpio_get_status_value(const char* status_name);

///  Set the target address of the GPIO chipset
void libgpio_set_gpio_base_address(libgpio_t* self, int GPx_base_index);

///  Set the offset to access GPI pins
void libgpio_set_gpi_offset(libgpio_t* self, int gpi_offset);

///  Set the offset to access GPO pins
void libgpio_set_gpo_offset(libgpio_t* self, int gpo_offset);

///  Set the number of supported GPI
void libgpio_set_gpi_count(libgpio_t* self, int gpi_count);

///  Get the number of supported GPI
int libgpio_get_gpi_count();

///  Set the number of supported GPO
void libgpio_set_gpo_count(libgpio_t* self, int gpo_count);

///  Get the number of supported GPO
int libgpio_get_gpo_count();

/// Add mapping GPI number -> HW pin number
void libgpio_add_gpi_mapping(libgpio_t* self, int port_num, int pin_num);

/// Add mapping GPO number -> HW pin number
void libgpio_add_gpo_mapping(libgpio_t* self, int port_num, int pin_num);

///  Set the test mode
void libgpio_set_test_mode(libgpio_t* self, bool test_mode);

///  Set the verbosity
void libgpio_set_verbose(libgpio_t* self, bool verbose);

///  Destroy the libgpio
void libgpio_destroy(libgpio_t** self_p);
