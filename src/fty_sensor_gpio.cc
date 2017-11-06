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
// * Add zuuid to GPO_INTERACTION too
// * Ensure we don't return OK/ERROR
// * Reenable tests for -server
// * Location of "data" directory into a variable like other components
//   (even if a hardcode for starters); use configure-script data preferably
// * Documentation
// ** actors as per https://github.com/zeromq/czmq/blob/master/src/zconfig.c#L15
// * MAILBOX REQ handling:
// * cleanup, final cppcheck, fix all FIXMEs...
// * Support for fine grained pin mapping in config file
//   gpo_mapping
//       <gpo number> = <pin number>
//   gpi_mapping
//       <gpi number> = <pin number>
// To be discussed:
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

// Send an update request over the MQ to check for GPIO status
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
    const char *state_file = NULL;
    char* actor_name = NULL;
    char* endpoint = NULL;
    const char* str_poll_interval = NULL;
    int poll_interval = DEFAULT_POLL_INTERVAL;
    const char* gpio_base_address = "-1";
    const char* gpi_offset = "0";
    const char* gpi_count = "0";
    const char* gpo_offset = "0";
    const char* gpo_count = "0";
    char ***gpo_mapping = new char**[2];
    gpo_mapping[0] = (char **) malloc (sizeof (char**));
    gpo_mapping[1] = (char **) malloc (sizeof (char**));
    char ***gpi_mapping = new char**[2];
    gpi_mapping[0] = (char **) malloc (sizeof (char**));
    gpi_mapping[1] = (char **) malloc (sizeof (char**));
    int gpi_mapping_count = 0;
    int gpo_mapping_count = 0;
    int gpi_mapping_size = 1;
    int gpo_mapping_size = 1;
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
        else if (streq (argv [argn], "--endpoint") || streq (argv [argn], "-e")) {
            if (param) endpoint = strdup(param);
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
        // State file
        state_file = s_get (config, "server/statefile", "/var/lib/fty/fty-sensor-gpio/state");
        // Polling interval
        str_poll_interval = s_get (config, "server/check_interval", "2000");
        if (str_poll_interval) {
            poll_interval = atoi(str_poll_interval);
        }
        // Target address of the GPIO chipset
        gpio_base_address = s_get (config, "hardware/gpio_base_address", "-1");

        // GPO configuration
        gpo_offset = s_get (config, "hardware/gpo_offset", "0");
        gpo_count = s_get (config, "hardware/gpo_count", "0");
        zconfig_t *item = zconfig_locate(config, "hardware/gpo_mapping");
        if (NULL != item) {
            item = zconfig_child(item);
            while (NULL != item) {
                if (gpo_mapping_size == gpo_mapping_count) {
                    char **tmp = NULL;
                    gpo_mapping_size *= 2;
                    tmp = (char **)realloc(gpo_mapping[0], sizeof(char *) * gpo_mapping_size);
                    if (NULL == tmp) {
                        zsys_error("gpo_mapping reallocation failed, OOM!");
                        return -1;
                    }
                    gpo_mapping[0] = tmp;
                    tmp = (char **)realloc(gpo_mapping[1], sizeof(char *) * gpo_mapping_size);
                    if (NULL == tmp) {
                        zsys_error("gpo_mapping reallocation failed, OOM!");
                        return -1;
                    }
                    gpo_mapping[1] = tmp;
                }
                my_zsys_debug ("Key = %s, value = %s", zconfig_name (item), zconfig_value (item));
                gpo_mapping[0][gpo_mapping_count] = strdup(zconfig_name(item));
                gpo_mapping[1][gpo_mapping_count] = strdup(zconfig_value(item));
                ++gpo_mapping_count;
                item = zconfig_next(item);
            }
        }

        // GPI configuration
        gpi_offset = s_get (config, "hardware/gpi_offset", "0");
        gpi_count = s_get (config, "hardware/gpi_count", "0");
        item = zconfig_locate(config, "hardware/gpi_mapping");
        if (NULL != item) {
            item = zconfig_child(item);
            while (NULL != item) {
                if (gpi_mapping_size == gpi_mapping_count) {
                    char **tmp = NULL;
                    gpi_mapping_size *= 2;
                    tmp = (char **)realloc(gpi_mapping[0], sizeof(char *) * gpi_mapping_size);
                    if (NULL == tmp) {
                        zsys_error("gpi_mapping reallocation failed, OOM!");
                        return -1;
                    }
                    gpi_mapping[0] = tmp;
                    tmp = (char **)realloc(gpi_mapping[1], sizeof(char *) * gpi_mapping_size);
                    if (NULL == tmp) {
                        zsys_error("gpi_mapping reallocation failed, OOM!");
                        return -1;
                    }
                    gpi_mapping[1] = tmp;
                }
                my_zsys_debug ("Key = %s, value = %s", zconfig_name (item), zconfig_value (item));
                gpi_mapping[0][gpi_mapping_count] = strdup(zconfig_name(item));
                gpi_mapping[1][gpi_mapping_count] = strdup(zconfig_value(item));
                ++gpi_mapping_count;
                item = zconfig_next(item);
            }
        }

        my_zsys_debug (verbose, "Polling interval set to %i", poll_interval);
        if (endpoint) zstr_free(&endpoint);
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
        template_filename = string("./selftest-ro/data/") + string("DCS001.tpl");
        template_file = fopen(template_filename.c_str(), "r");
        if (!template_file) {
            template_filename = string("./src/data/") + string("DCS001.tpl");
            template_file = fopen(template_filename.c_str(), "r");
            if (!template_file) {
                template_dir = NULL;
                zsys_error ("Can't find sensors template files directory!");
                zstr_free(&actor_name);
                zstr_free(&endpoint);
                return EXIT_FAILURE;
            }
            else {
                // Running from the top level directory
                template_dir = strdup("./src/data/");
            }
        }
        else {
            // Running from src/ directory
            template_dir = strdup("./data/");
        }
    }
    else {
        // Running from installed package
        template_dir = strdup("/usr/share/fty-sensor-gpio/data/");
    }
    fclose(template_file);
    my_zsys_debug (verbose, "Using sensors template directory: %s", template_dir);

    // Check env. variables
    if (getenv ("BIOS_LOG_LEVEL") && streq (getenv ("BIOS_LOG_LEVEL"), "LOG_DEBUG"))
        verbose = true;

    zactor_t *assets = zactor_new (fty_sensor_gpio_assets, (void*)"gpio-assets");
    zactor_t *server = zactor_new (fty_sensor_gpio_server, (void*)actor_name);

    if (verbose) {
        zstr_sendx (server, "VERBOSE", NULL);
        zstr_sendx (assets, "VERBOSE", NULL);
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
    int i = 0;
    for (i = 0; i < gpi_mapping_count; ++i) {
        zstr_sendx (server, "GPI_MAPPING", gpi_mapping[0][i], gpi_mapping[1][i], NULL);
    }
    for (i = 0; i < gpo_mapping_count; ++i) {
        zstr_sendx (server, "GPO_MAPPING", gpo_mapping[0][i], gpo_mapping[1][i], NULL);
    }

    // Setup an update event message every x microseconds, to check GPI status
    zloop_t *gpio_status_update = zloop_new();
    zloop_timer (gpio_status_update, poll_interval, 0, s_update_event, server);
    zloop_start (gpio_status_update);

    // Cleanup
    zloop_destroy (&gpio_status_update);
    zactor_destroy (&server);
    zactor_destroy (&assets);
    zstr_free(&template_dir);
    zstr_free(&actor_name);
    zstr_free(&endpoint);
    zconfig_destroy (&config);
    free(gpi_mapping[0]);
    free(gpi_mapping[1]);
    free(gpo_mapping[0]);
    free(gpo_mapping[1]);

    return 0;
}
