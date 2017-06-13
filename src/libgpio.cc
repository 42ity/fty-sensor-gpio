/*  =========================================================================
    libgpio - General Purpose Input/Output (GPIO) sensors library

    Copyright (C) 2014 - 2017 Eaton                                        
                                                                           
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

/*
@header
    libgpio - General Purpose Input/Output (GPIO) sensors library
@discuss
@end
*/

#include "fty_sensor_gpio_classes.h"

//  Structure of our class

struct _libgpio_t {
    int  gpio_base_address;  // Base address of the GPIOs chipset
    bool test_mode;          // true if we are in test mode, false otherwise
    bool verbose;            // is actor verbose or not
    int  gpo_offset;         // offset to access GPO pins
    int  gpi_offset;         // offset to access GPI pins
    int  gpo_count;          // number of supported GPO
    int  gpi_count;          // number of supported GPI
};

//  Private functions forward declarations

static int libgpio_export(libgpio_t *self, int pin);
static int libgpio_unexport(libgpio_t *self, int pin);
static int libgpio_set_direction(libgpio_t *self, int pin, int dir);
static int mkpath(char* file_path, mode_t mode);
// FIXME: use zsys_dir_create (...);

//  Test mode variables
const char *SELFTEST_DIR_RO = "src/selftest-ro";
const char *SELFTEST_DIR_RW = "src/selftest-rw";

//  --------------------------------------------------------------------------
//  Create a new libgpio

libgpio_t *
libgpio_new (void)
{
    libgpio_t *self = (libgpio_t *) zmalloc (sizeof (libgpio_t));
    assert (self);
    //  Initialize class properties here
    self->gpio_base_address = GPIO_BASE_INDEX;
    self->gpo_offset = 0;
    self->gpi_offset = 0;
    self->gpo_count = 0;
    self->gpi_count = 0;
    self->test_mode = false;
    self->verbose = false;

    return self;
}

//  --------------------------------------------------------------------------
//  Set the target address of the GPIO chipset

void
libgpio_set_gpio_base_address (libgpio_t *self, int GPx_base_index)
{
    my_zsys_debug (self->verbose, "%s: setting address to %i", __func__, GPx_base_index);
    self->gpio_base_address = GPx_base_index;
}

//  --------------------------------------------------------------------------
//  Set the offset to access GPO pins

void
libgpio_set_gpo_offset (libgpio_t *self, int gpo_offset)
{
    my_zsys_debug (self->verbose, "%s: setting GPO offset to %i", __func__, gpo_offset);
    self->gpo_offset = gpo_offset;
}

//  --------------------------------------------------------------------------
//  Set the offset to access GPI pins

void
libgpio_set_gpi_offset (libgpio_t *self, int gpi_offset)
{
    my_zsys_debug (self->verbose, "%s: setting GPI offset to %i", __func__, gpi_offset);
    self->gpi_offset = gpi_offset;
}

//  --------------------------------------------------------------------------
//  Set the number of supported GPI
void
libgpio_set_gpi_count (libgpio_t *self, int gpi_count)
{
    my_zsys_debug (self->verbose, "%s: setting GPI count to %i", __func__, gpi_count);
    self->gpi_count = gpi_count;
}

//  --------------------------------------------------------------------------
//  Set the number of supported GPO
void
libgpio_set_gpo_count (libgpio_t *self, int gpo_count)
{
    my_zsys_debug (self->verbose, "%s: setting GPO count to %i", __func__, gpo_count);
    self->gpo_count = gpo_count;
}


//  --------------------------------------------------------------------------
//  Set the test mode

void
libgpio_set_test_mode (libgpio_t *self, bool test_mode)
{
    my_zsys_debug (self->verbose, "%s: setting test_mode to '%s'", __func__, (test_mode == true)?"True":"False");
    self->test_mode = test_mode;
}

//  --------------------------------------------------------------------------
//  Set the verbosity

void
libgpio_set_verbose (libgpio_t *self, bool verbose)
{
    my_zsys_debug (self->verbose, "%s: setting verbose to '%s'", __func__, (verbose == true)?"True":"False");
    self->verbose = verbose;
}

//  --------------------------------------------------------------------------
//  Read a GPI or GPO status
int
libgpio_read (libgpio_t *self, int GPx_number, int direction)
{
    char path[GPIO_VALUE_MAX];
    char value_str[3];
    int fd;

    memset(&value_str[0], 0, 3);

    // Sanity check
    if (GPx_number > self->gpi_count) {
        zsys_error("Requested GPx is higher than the count of supported GPIO!");
        return -1;
    }

    int pin = (GPx_number + self->gpi_offset);

    my_zsys_debug (self->verbose, "%s: reading GPx #%i (pin %i)", __func__, GPx_number, pin);

    // Enable the desired GPIO
    if (libgpio_export(self, pin) == -1)
        return -1;

    // Set its direction
    if (libgpio_set_direction(self, pin, direction) == -1)
        return -1;

    snprintf(path, GPIO_VALUE_MAX, "%s/sys/class/gpio/gpio%d/value",
        (self->test_mode)?SELFTEST_DIR_RW:"", // trick #1 to allow testing
        pin + self->gpio_base_address);
    // trick #2 to allow testing
    if (self->test_mode)
        mkpath(path, 0777);
    fd = open(path, O_RDONLY | ((self->test_mode)?O_CREAT:0), 0777);
    if (fd == -1) {
        zsys_error("Failed to open gpio '%s' for reading!", path);
        return -1;
    }

    if (read(fd, value_str, 3) <= 0) {
        zsys_error("Failed to read value!");
        close(fd);
        return -1;
    }

    my_zsys_debug (self->verbose, "%s: result %s", __func__, value_str);

    close(fd);

    if (libgpio_unexport(self, pin) == -1) {
        return -1;
    }

    return(atoi(&value_str[0]));
}
//  --------------------------------------------------------------------------
//  Write a GPO (to enable or disable it)
int
libgpio_write (libgpio_t *self, int GPO_number, int value)
{
    static const char s_values_str[] = "01";
    char path[GPIO_VALUE_MAX];
    int fd;
    int retval = 0;

    // Sanity check
    if (GPO_number > self->gpo_count) {
        zsys_error("Requested GPx is higher than the count of supported GPIO!");
        return -1;
    }

    int pin = (GPO_number + self->gpo_offset);

    my_zsys_debug (self->verbose, "%s: writing GPO #%i (pin %i)", __func__, GPO_number, pin);

    // Enable the desired GPIO
    if (libgpio_export(self, pin) == -1)
        return -1;

    // Set its direction
    if (libgpio_set_direction(self, pin, GPIO_DIRECTION_OUT) == -1)
        return -1;

    snprintf(path, GPIO_VALUE_MAX, "%s/sys/class/gpio/gpio%d/value",
        (self->test_mode)?SELFTEST_DIR_RW:"", // trick #1 to allow testing
        pin + self->gpio_base_address);
    // trick #2 to allow testing
    if (self->test_mode)
        mkpath(path, 0777);
    fd = open(path, O_WRONLY | ((self->test_mode)?O_CREAT:0), 0777);
    if (fd == -1) {
        zsys_error("Failed to open gpio value for writing (path: %s)!", path);
        return(-1);
    }

    if (write(fd, &s_values_str[GPIO_STATE_CLOSED == value ? 0 : 1], 1) != 1) {
        zsys_error("Failed to write value!");
        retval = -1;
    }

    my_zsys_debug (self->verbose, "%s: result %i", __func__, retval);

    close(fd);

    if (libgpio_unexport(self, pin) == -1) {
        retval = -1;
    }
    return retval;
}


//  --------------------------------------------------------------------------
//  Get the textual name for a status
const string
libgpio_get_status_string (int value)
{
    string status_str;

    switch (value) {
        case GPIO_STATE_CLOSED:
            status_str = "closed";
            break;
        case GPIO_STATE_OPENED:
            status_str = "opened";
            break;
        case GPIO_STATE_UNKNOWN:
        default:
            status_str = ""; // FIXME: return "unknown"?
    }
    return status_str;
}

//  --------------------------------------------------------------------------
//  Get the numeric value for a status name
int
libgpio_get_status_value (const char* status_name)
{
    int status_value = GPIO_STATE_CLOSED;

    if ( (streq (status_name, "closed")) || (streq (status_name, "close")) ) {
        status_value = GPIO_STATE_CLOSED;
    }
    else if ( (streq (status_name, "opened")) || (streq (status_name, "open")) ) {
        status_value = GPIO_STATE_OPENED;
    }
    else
        status_value = GPIO_STATE_UNKNOWN;

    return status_value;
}

//  --------------------------------------------------------------------------
//  Destroy the libgpio

void
libgpio_destroy (libgpio_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        libgpio_t *self = *self_p;
        //  Free class properties here
        //  Free object itself
        free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Self test of this class

void
libgpio_test (bool verbose)
{
    printf (" * libgpio: ");

    // Note: for testing purpose, we use a trick, that is to access the /sys
    // FS under our SELFTEST_DIR_RW.

    //  @selftest
    //  Simple create/destroy test
    libgpio_t *self = libgpio_new ();
    assert (self);
    libgpio_destroy (&self);

    // Setup
    self = libgpio_new ();
    assert (self);
    //libgpio_set_verbose (self, true);
    libgpio_set_test_mode (self, true);
    libgpio_set_gpio_base_address (self, 0);
    // We use the same offset for GPI and GPO, to be able to write a GPO
    // and read the same for GPI
    libgpio_set_gpi_offset (self, 0);
    libgpio_set_gpo_offset (self, 0);
    libgpio_set_gpi_count (self, 10);
    libgpio_set_gpo_count (self, 5);

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase for the variables (assert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    // Uncomment these to use C++ strings in C++ selftest code:
    //std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    //std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);
    //assert ( (str_SELFTEST_DIR_RO != "") );
    //assert ( (str_SELFTEST_DIR_RW != "") );
    // NOTE that for "char*" context you need (str_SELFTEST_DIR_RO + "/myfilename").c_str()

    // Common pattern for plain C:
    // char *test_state_file = zsys_sprintf ("%s/state_file", SELFTEST_DIR_RW);
    // assert (test_state_file != NULL);
    // zstr_sendx (fs, "STATE_FILE", test_state_file, NULL);
    // zstr_free (&test_state_file);

    // Write test
    // Let's first write to the dummy GPIO, so that the read works afterward
    assert( libgpio_write (self, 1, GPIO_STATE_CLOSED) == 0);

    // Read test
    assert( libgpio_read (self, 1, GPIO_DIRECTION_IN) == GPIO_STATE_CLOSED );

    // Value resolution test
    assert( libgpio_get_status_value("opened") == GPIO_STATE_OPENED );
    assert( libgpio_get_status_value("closed") == GPIO_STATE_CLOSED );
    assert( libgpio_get_status_value( libgpio_get_status_string(GPIO_STATE_CLOSED).c_str() ) == GPIO_STATE_CLOSED );

    // Delete all test files
    std::string sys_fn = string(SELFTEST_DIR_RW) + "/sys";
    zdir_t *dir = zdir_new (sys_fn.c_str(), NULL);
    assert (dir);
    zdir_remove (dir, true);
    zdir_destroy (&dir);

    libgpio_destroy (&self);

    //  @end
    printf ("OK\n");
}

//  --------------------------------------------------------------------------
//  Private functions

//  --------------------------------------------------------------------------
//  Set the current GPIO pin to act on

int
libgpio_export(libgpio_t *self, int pin)
{
    char buffer[GPIO_BUFFER_MAX];
    char path[GPIO_VALUE_MAX];
    ssize_t bytes_written;
    int fd;
    int retval = 0;

    snprintf(path, GPIO_VALUE_MAX, "%s/sys/class/gpio/export",
        (self->test_mode)?SELFTEST_DIR_RW:""); // trick #1 to allow testing
    // trick #2 to allow testing
    if (self->test_mode)
        mkpath(path, 0777);
    fd = open(path, O_WRONLY | ((self->test_mode)?O_CREAT:0), 0777);
    if (fd == -1) {
        zsys_error("%s: Failed to open %s for writing! %i", __func__, path, errno);
        return -1;
    }

    bytes_written = snprintf(buffer, GPIO_BUFFER_MAX, "%d", pin + self->gpio_base_address);
    if (write(fd, buffer, bytes_written) < bytes_written) {
        my_zsys_debug (self->verbose, "%s: ERROR: wrote less than %i bytes",
            __func__, bytes_written);
        retval = -1;
    }

    close(fd);
    return retval;
}

//  --------------------------------------------------------------------------
//  Unset the current GPIO pin to act on

int
libgpio_unexport(libgpio_t *self, int pin)
{
    char buffer[GPIO_BUFFER_MAX];
    char path[GPIO_VALUE_MAX];
    ssize_t bytes_written;
    int fd;
    int retval = 0;

    snprintf(path, GPIO_VALUE_MAX, "%s/sys/class/gpio/unexport",
        (self->test_mode)?SELFTEST_DIR_RW:""); // trick #1 to allow testing
    // trick #2 to allow testing
    if (self->test_mode)
        mkpath(path, 0777);
    fd = open(path, O_WRONLY | ((self->test_mode)?O_CREAT:0), 0777);
    if (fd == -1) {
      zsys_error("Failed to open unexport for writing!");
      return -1;
    }

    bytes_written = snprintf(buffer, GPIO_BUFFER_MAX, "%d", pin + self->gpio_base_address);
    if (write(fd, buffer, bytes_written) < bytes_written) {
        my_zsys_debug (self->verbose, "%s: ERROR: wrote less than %i bytes",
            __func__, bytes_written);
        retval = -1;
    }

    close(fd);
    return retval;
}


//  --------------------------------------------------------------------------
//  Set the current GPIO direction to 'in' (read) or 'out' (write)

int
libgpio_set_direction(libgpio_t *self, int pin, int direction)
{
    static const char s_directions_str[]  = "in\0out";
    int retval = 0;

    char path[GPIO_DIRECTION_MAX];
    int fd;

    snprintf(path, GPIO_DIRECTION_MAX, "%s/sys/class/gpio/gpio%d/direction",
        ((self->test_mode)?SELFTEST_DIR_RW:""), // trick #1 to allow testing
        pin + self->gpio_base_address);
    // trick #2 to allow testing
    if (self->test_mode)
        mkpath(path, 0777);
    fd = open(path, O_WRONLY | ((self->test_mode)?O_CREAT:0), 0777);
    if (fd == -1) {
        zsys_error("%s: Failed to open %s for writing!", __func__, path);
        return -1;
    }

    if (write(fd, &s_directions_str[GPIO_DIRECTION_IN == direction ? 0 : 3],
      GPIO_DIRECTION_IN == direction ? 2 : 3) == -1) {
        zsys_error("%s: Failed to set direction!", __func__);
        retval = -1;
    }

    close(fd);
    return retval;
}

//  --------------------------------------------------------------------------
//  Helper function to recursively create directories

// FIXME: replace with zsys_dir_create (const char *pathname, ...);
static int
mkpath(char* file_path, mode_t mode)
{
    assert(file_path && *file_path);
    char* p;
    for (p=strchr(file_path+1, '/'); p; p=strchr(p+1, '/')) {
        *p='\0';
        if (mkdir(file_path, mode)==-1) {
            if (errno!=EEXIST) {
                *p='/';
                return -1;
            }
        }
        *p='/';
    }
    return 0;
}
