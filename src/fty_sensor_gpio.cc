/*  =========================================================================
    fty_sensor_gpio - Manage GPI sensors and GPO devices

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
    fty_sensor_gpio - Manage GPI sensors and GPO devices
@discuss
@end
*/

#include "fty_sensor_gpio_classes.h"

// TODO:
// * Smart update of existing entries
// * Tests for _server
// * Location of "data" directory into a variable like other components
//   (even if a hardcode for starters); use configure-script data preferably
// * Documentation
// ** README.md
// ** actors as per https://github.com/zeromq/czmq/blob/master/src/zconfig.c#L15
// * MAILBOX REQ handling:
// ** Sensors manifest request with empty sensors list (return all)
// ** Sensor template addition (create local files with all details provided through UI/CLI)
// * cleanup, final cppcheck, fix all FIXMEs...
// * Support for fine grained pin mapping in config file
//   gpo_mapping
//       <gpo number> = <pin number>
//   gpi_mapping
//       <gpi number> = <pin number>
// To be discussed:
// * Add 'location' / parent.name (+variable $location)
//   location is the IPC, we want the installation location (which rack door, ...)
// * Check for convergence with other dry-contacts (on EMP001 and fty-sensor-env, EMP002, 
// * Add 'int last_state' to '_gpx_info_t' and only publish state change?
//   (i.e. last_state != current_state)
// * i18n for alerts and $status

void
usage(){
    puts ("fty-sensor-gpio [options] ...");
    puts ("  -v|--verbose        verbose test output");
    puts ("  -h|--help           this information");
    puts ("  -c|--config         path to config file\n");
    puts ("  -e|--endpoint       malamute endpoint [ipc://@/malamute]");

}

// Send an update request over the MQ to check for GPIO status & alerts
static int
s_update_event (zloop_t *loop, int timer_id, void *output)
{
    zstr_send (output, "UPDATE");
    return 0;
}

int main (int argc, char *argv [])
{
    char *config_file = NULL;
    zconfig_t *config = NULL;
    char* actor_name = NULL;
    char* endpoint = NULL;
    const char* str_poll_interval = NULL;
    int poll_interval = DEFAULT_POLL_INTERVAL;
    const char* gpio_base_address = "-1";
    const char* gpi_offset = "0";
    const char* gpi_count = "0";
    const char* gpo_offset = "0";
    const char* gpo_count = "0";
    bool verbose = false;
    int argn;

    // Parse command line
    for (argn = 1; argn < argc; argn++) {
        char *param = NULL;
        if (argn < argc - 1) param = argv [argn+1];

        if (streq (argv [argn], "--help")
        ||  streq (argv [argn], "-h")) {
            usage();
            return 0;
        }
        else if (streq (argv [argn], "--verbose") || streq (argv [argn], "-v")) {
            verbose = true;
        }
        else if (streq (argv [argn], "--config") || streq (argv [argn], "-c")) {
            if (param) config_file = param;
            ++argn;
        }
        else {
            // FIXME: as per the systemd service file, the config file
            // is provided as the default arg without '-c'!
            // So, should we consider this?
            printf ("Unknown option: %s\n", argv [argn]);
            return 1;
        }
    }

    // Parse config file
    if(config_file) {
        my_zsys_debug (verbose, "fty_sensor_gpio: loading configuration file '%s'", config_file);
        config = zconfig_load (config_file);
        if (!config) {
            zsys_error ("Failed to load config file %s: %m", config_file);
            exit (EXIT_FAILURE);
        }
        // VERBOSE
        if (streq (zconfig_get (config, "server/verbose", "false"), "true")) {
            verbose = true;
        }
        // Polling interval
        str_poll_interval = s_get (config, "server/poll_interval", "2000");
        if (str_poll_interval) {
            poll_interval = atoi(str_poll_interval);
        }
        // Target address of the GPIO chipset
        gpio_base_address = s_get (config, "hardware/gpio_base_address", "-1");

        // GPO configuration
        gpo_offset = s_get (config, "hardware/gpo_offset", "0");
        gpo_count = s_get (config, "hardware/gpo_count", "0");

        // GPI configuration
        gpi_offset = s_get (config, "hardware/gpi_offset", "0");
        gpi_count = s_get (config, "hardware/gpi_count", "0");

        my_zsys_debug (verbose, "Polling interval set to %i", poll_interval);
        endpoint = strdup(s_get (config, "malamute/endpoint", NULL));
        actor_name = strdup(s_get (config, "malamute/address", NULL));
    }
    if (actor_name == NULL)
        actor_name = strdup(FTY_SENSOR_GPIO_AGENT);

    if (endpoint == NULL)
        endpoint = strdup("ipc://@/malamute");

    // Guess the template installation directory
    char *template_dir = NULL;
    string template_filename = string("/usr/share/fty-sensor-gpio/data/") + string("DCS001.tpl");
    FILE *template_file = fopen(template_filename.c_str(), "r");
    if (!template_file) {
        template_filename = string("./data/") + string("DCS001.tpl");
        template_file = fopen(template_filename.c_str(), "r");
        if (!template_file) {
            template_dir = NULL;
            zsys_error ("Can't find sensors template files directory!");
            return EXIT_FAILURE;
        }
        else {
            template_dir = strdup("./data/");
        }
    }
    else {
        template_dir = strdup("/usr/share/fty-sensor-gpio/data/");
    }
    fclose(template_file);
    my_zsys_debug (verbose, "Using sensors template directory: %s", template_dir);

    // Check env. variables
    if (getenv ("BIOS_LOG_LEVEL") && streq (getenv ("BIOS_LOG_LEVEL"), "LOG_DEBUG"))
        verbose = true;

    zactor_t *assets = zactor_new (fty_sensor_gpio_assets, (void*)"gpio-assets");
    zactor_t *server = zactor_new (fty_sensor_gpio_server, (void*)actor_name);
    zactor_t *alerts = zactor_new (fty_sensor_gpio_alerts, (void*)"gpio-alerts");

    if (verbose) {
        zstr_sendx (server, "VERBOSE", NULL);
        zstr_sendx (assets, "VERBOSE", NULL);
        zstr_sendx (alerts, "VERBOSE", NULL);
        zsys_info ("%s - Agent which manages GPI sensors and GPO devices", actor_name);
    }

    // 1rst stream to handle assets
    zstr_sendx (assets, "CONNECT", endpoint, NULL);
    zstr_sendx (assets, "PRODUCER", FTY_PROTO_STREAM_ASSETS, NULL);
    zstr_sendx (assets, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);
    zstr_sendx (assets, "TEMPLATE_DIR", template_dir, NULL);

    // 2nd (main) stream to handle GPx polling, metrics publication and mailbox requests
    zstr_sendx (server, "CONNECT", endpoint, NULL);
    zstr_sendx (server, "PRODUCER", FTY_PROTO_STREAM_METRICS_SENSOR, NULL);
    zstr_sendx (server, "TEMPLATE_DIR", template_dir, NULL);
    if (!streq(gpio_base_address, "-1")) {
        my_zsys_debug (verbose, "Target address of the GPIO chipset set to %s", gpio_base_address);
        zstr_sendx (server, "GPIO_CHIP_ADDRESS", gpio_base_address, NULL);
    }
    zstr_sendx (server, "GPI_OFFSET", gpi_offset, NULL);
    zstr_sendx (server, "GPO_OFFSET", gpo_offset, NULL);
    zstr_sendx (server, "GPI_COUNT", gpi_count, NULL);
    zstr_sendx (server, "GPO_COUNT", gpo_count, NULL);

    // 3rd stream to publish and manage alerts
    zstr_sendx (alerts, "CONNECT", endpoint, NULL);
    zstr_sendx (alerts, "PRODUCER", FTY_PROTO_STREAM_ALERTS_SYS, NULL);

    // Setup an update event message every x seconds, to check GPI status & alerts
    zloop_t *gpio_status_update = zloop_new();
    zloop_timer (gpio_status_update, poll_interval, 0, s_update_event, server);
    zloop_timer (gpio_status_update, poll_interval, 0, s_update_event, alerts);
    zloop_start (gpio_status_update);

    // Cleanup
    zloop_destroy (&gpio_status_update);
    zactor_destroy (&server);
    zactor_destroy (&assets);
    zactor_destroy (&alerts);
    zstr_free(&template_dir);
    zstr_free(&actor_name);
    zstr_free(&endpoint);
    zconfig_destroy (&config);

    return 0;
}
