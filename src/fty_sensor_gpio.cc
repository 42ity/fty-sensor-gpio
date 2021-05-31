/*  =========================================================================
    fty_sensor_gpio - Manage GPI sensors and GPO devices

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

/*
@header
    fty_sensor_gpio - Manage GPI sensors and GPO devices
@discuss
@end
*/

#include "fty_sensor_gpio.h"
#include "fty_sensor_gpio_server.h"
#include "fty_sensor_gpio_assets.h"
#include <fty_proto.h>
#include <fty_log.h>

// TODO:
// * Smart update of existing entries
// * Ensure we don't return OK/ERROR
// * Location of "data" directory into a variable like other components
//   (even if a hardcode for starters); use configure-script data preferably
// * Documentation
// ** actors as per https://github.com/zeromq/czmq/blob/master/src/zconfig.c#L15
// * MAILBOX REQ handling:
// * cleanup, final cppcheck, fix all FIXMEs...
// To be discussed:
// * Check for convergence with other dry-contacts (on EMP001 and fty-sensor-env, EMP002,
// * Add 'int last_state' to '_gpx_info_t' and only publish state change?
//   (i.e. last_state != current_state)
// * i18n for alerts and $status

void
usage(){
    puts ("fty-sensor-gpio [options] ...");
    puts ("  -v|--verbose        verbose output");
    puts ("  -h|--help           this information");
    puts ("  -c|--config         path to agent config file\n");
    puts ("  -e|--endpoint       malamute endpoint [ipc://@/malamute]");

}

// Send an update request over the MQ to check for GPIO status
static int
s_update_event (zloop_t *loop, int timer_id, void *output)
{
    // Please the compiler
    if (loop && timer_id)
        zstr_send (output, "UPDATE");
    return 0;
}

// Schedule HW_CAP request to do the initial configuration for local GPI/GPO
static int
s_request_hwcap_event (zloop_t *loop, int timer_id, void *output)
{
    if (!hw_cap_inited) {
        zstr_send (output, "HW_CAP");
        return 0;
    }
    else // we can stop this timer, valid reply received
        return zloop_timer_end (loop, timer_id);
}

// Condition asset actor on the successful configuration of server actor (HW_CAP)
static int
s_server_ready_event (zloop_t *loop, int timer_id, void *output)
{
    if (hw_cap_inited) {
        zstr_sendx (output, "PRODUCER", FTY_PROTO_STREAM_ASSETS, NULL);
        zstr_sendx (output, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);
        // we can now stop this timer
        return zloop_timer_end (loop, timer_id);
    }
    else // continue to loop
        return 0;
}

int main (int argc, char *argv [])
{
    char *config_file = NULL;
    zconfig_t *config = NULL;
    char *state_file = NULL;
    char* actor_name = NULL;
    char* endpoint = NULL;
    const char* str_poll_interval = NULL;
    int poll_interval = DEFAULT_POLL_INTERVAL;
    bool verbose = false;
    int argn;
    char *log_config = NULL;

    ManageFtyLog::setInstanceFtylog(FTY_SENSOR_GPIO_AGENT);

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
        log_debug ("fty_sensor_gpio: loading configuration file '%s'", config_file);
        config = zconfig_load (config_file);
        if (!config) {
            log_error ("Failed to load config file %s: %m", config_file);
            exit (EXIT_FAILURE);
        }

        if (streq (zconfig_get (config, "server/verbose", "false"), "true")) {
            verbose = true;
        }
        // State file
        state_file = strdup(s_get (config, "server/statefile", DEFAULT_STATEFILE_PATH));
        // Polling interval
        str_poll_interval = s_get (config, "server/check_interval", "2000");
        if (str_poll_interval) {
            poll_interval = atoi(str_poll_interval);
        }
        log_debug ("Polling interval set to %i", poll_interval);
        if (endpoint) zstr_free(&endpoint);
        endpoint = strdup(s_get (config, "malamute/endpoint", NULL));
        actor_name = strdup(s_get (config, "malamute/address", NULL));
        log_config = strdup(s_get (config, "log/config", DEFAULT_LOG_CONFIG));
    }
    if (actor_name == NULL)
        actor_name = strdup(FTY_SENSOR_GPIO_AGENT);

    if (endpoint == NULL)
        endpoint = strdup("ipc://@/malamute");

    if (state_file == NULL)
        state_file = strdup(DEFAULT_STATEFILE_PATH);

    if (log_config)
        ManageFtyLog::getInstanceFtylog()->setConfigFile(std::string(log_config));

    if (verbose)
        ManageFtyLog::getInstanceFtylog()->setVeboseMode();

    // Guess the template installation directory
    char *template_dir = NULL;
    std::string template_filename = std::string("/usr/share/fty-sensor-gpio/data/") + "DCS001.tpl";
    FILE *template_file = fopen(template_filename.c_str(), "r");
    if (!template_file) {
        template_filename = std::string("tests/selftest-ro/data/") + "DCS001.tpl";
        template_file = fopen(template_filename.c_str(), "r");
        if (!template_file) {
            template_filename = std::string("./src/data/") + "DCS001.tpl";
            template_file = fopen(template_filename.c_str(), "r");
            if (!template_file) {
                template_dir = NULL;
                log_error ("Can't find sensors template files directory!");
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
    log_debug ("Using sensors template directory: %s", template_dir);


    zactor_t *server = zactor_new (fty_sensor_gpio_server, actor_name);
    zactor_t *assets = zactor_new (fty_sensor_gpio_assets, const_cast<char*>("gpio-assets"));

    log_info ("%s - Agent which manages GPI sensors and GPO devices", actor_name);

    // 1rst (main) stream to handle GPx polling, metrics publication and mailbox requests
    // -server MUST be init'ed prior to -asset
    zstr_sendx (server, "CONNECT", endpoint, NULL);
    zstr_sendx (server, "PRODUCER", FTY_PROTO_STREAM_METRICS_SENSOR, NULL);
    zstr_sendx (server, "TEMPLATE_DIR", template_dir, NULL);
    //zstr_sendx (server, "HW_CAP", NULL);
    zstr_sendx (server, "STATEFILE", state_file, NULL);

    // 2nd stream to handle assets
    zstr_sendx (assets, "TEMPLATE_DIR", template_dir, NULL);
    zstr_sendx (assets, "CONNECT", endpoint, NULL);

    // Setup:
    // * an update event message every x microseconds, to check GPI status
    // * a request event message every 5 seconds, to request local HW capabilities
    // * asset actor production/consumption when server actor has received local HW capabilities
    zloop_t *gpio_events = zloop_new();
    zloop_timer (gpio_events, size_t(poll_interval), 0, s_update_event, server);
    zloop_timer (gpio_events, 5000, 0, s_request_hwcap_event, server);
    zloop_timer (gpio_events, 2000, 0, s_server_ready_event, assets);
    zloop_start (gpio_events);

    // Cleanup
    zloop_destroy (&gpio_events);
    zactor_destroy (&server);
    zactor_destroy (&assets);
    zstr_free(&template_dir);
    zstr_free(&actor_name);
    zstr_free(&endpoint);
    zstr_free(&state_file);
    zstr_free(&log_config);
    zconfig_destroy (&config);

    return 0;
}
