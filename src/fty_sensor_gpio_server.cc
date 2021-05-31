/*  =========================================================================
    fty_sensor_gpio_server - Actor

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
    fty_sensor_gpio_server - Actor
@discuss
     GPIO PROTOCOL

     ------------------------------------------------------------------------
    ## GPO_INTERACTION

    REQ:
        subject: "GPO_INTERACTION"
        Message is a multipart std::string message

        <zuuid>/sensor/action              - apply action (open | close) on sensor (asset or ext name)
                                      beside from open and close, enable | enabled |opened | high
                                      and disable | disabled | closed | low are also supported

    REP:
        subject: "GPO_INTERACTION"
        Message is a multipart message:

        * <zuuid>/OK                         = action applied successfully
        * <zuuid>/ERROR/<reason>

        where:
            <zuuid> = info for REST API so it could match response to request
            <reason>          = ASSET_NOT_FOUND / SET_VALUE_FAILED / UNKNOWN_VALUE / BAD_COMMAND / ACTION_NOT_APPLICABLE

     ------------------------------------------------------------------------
    ## GPIO_MANIFEST

    REQ:
        subject: "GPIO_MANIFEST"
        Message is a multipart std::string message

        <zuuid>/<sensor 1 part number>/.../<sensor N part number>      - get information on sensor(s)

        where:
            <zuuid> = info for REST API so it could match response to request
            <sensor x part number>   = Part number of the sensor(s), to get information on
                                       when empty, return all supported sensors information

    REP:
        subject: "GPIO_MANIFEST"
        Message is a multipart message:

        * OK/<sensor 1 description>/.../<sensor N description> = non-empty
        * ERROR/<reason>

        where:
            <reason>                 = ASSET_NOT_FOUND / BAD_COMMAND
            <sensor N description> =
sensor_partnumber/manufacturer/type/normal_state/gpx_direction/alarm_severity/alarm_message

     ------------------------------------------------------------------------
    ## GPIO_MANIFEST_SUMMARY

    REQ:
        subject: "GPIO_MANIFEST_SUMMARY"
        Message is a multipart std::string message: <zuuid>

              - get the list of supported sensors
                this is a light version of GPIO_MANIFEST, only returning
                sensor_partnumber/manufacturer

    REP:
        subject: "GPIO_MANIFEST_SUMMARY"
        Message is a multipart message:

        * <zuuid>/OK/<sensor 1 description>/.../<sensor N description> = non-empty
        * <zuuid>/ERROR/<reason>

        where:
            <zuuid> = info for REST API so it could match response to request
            <reason>                 = ASSET_NOT_FOUND / BAD_COMMAND
            <sensor N description> = sensor_partnumber/manufacturer

     ------------------------------------------------------------------------
    ## GPIO_TEMPLATE_ADD

    REQ:
        subject: "GPIO_TEMPLATE_ADD"
        Message is a multipart std::string message

        <zuuid>/<sensor description>      - request the creation of a sensor template file

        where:
            <sensor description> =
sensor_partnumber/manufacturer/type/normal_state/gpx_direction/alarm_severity/alarm_message

    REP:
        subject: "GPIO_TEMPLATE_ADD"
        Message is a multipart message:

        * <zuuid>/OK
        * <zuuid>/ERROR/<reason>

        where:
            <zuuid> = info for REST API so it could match response to request
            <reason>             = ...

     ------------------------------------------------------------------------
    ## GPOSTATE

    REQ:
        subject: "GPOSTATE"
        Message is a multipart std::string message

        <asset_name>/<gpo_number>/<default_state>      - store GPO with this properties into cache

    REP:
        none
@end
*/

#include "fty_sensor_gpio_server.h"
#include "libgpio.h"
#include "fty_sensor_gpio.h"
#include <fty_log.h>
#include <fty_proto.h>
#include <malamute.h>
#include <regex>
#include <stdio.h>

// Structure for GPO state

struct gpo_state_t
{
    int gpo_number;
    int default_state;
    int last_action;
    int in_alert;
};

//  Structure of our class

struct _fty_sensor_gpio_server_t
{
    char*         name;         // actor name
    mlm_client_t* mlm;          // malamute client
    libgpio_t*    gpio_lib;     // GPIO library handle
    bool          test_mode;    // true if we are in test mode, false otherwise
    char*         template_dir; // Location of the template files
    zhashx_t*     gpo_states;
};
typedef struct _fty_sensor_gpio_server_t fty_sensor_gpio_server_t;

// Flag to share if HW capabilities were successfully received
bool hw_cap_inited = false;

// Declare our testing HW_CAP reply, to be able to manage our tests
zmsg_t* hw_cap_test_reply_gpi = nullptr;
zmsg_t* hw_cap_test_reply_gpo = nullptr;

// Configuration accessors
// FIXME: why do we need that? zconfig_get should already do this, no?
const char* s_get(zconfig_t* config, const char* key, std::string& dfl)
{
    assert(config);
    char* ret = zconfig_get(config, key, dfl.c_str());
    if (!ret || streq(ret, ""))
        return const_cast<char*>(dfl.c_str());
    return ret;
}

const char* s_get(zconfig_t* config, const char* key, const char* dfl)
{
    assert(config);
    char* ret = zconfig_get(config, key, dfl);
    if (!ret || streq(ret, ""))
        return dfl;
    return ret;
}

static void free_fn(void** self_ptr)
{
    if (!self_ptr || !*self_ptr) {
        log_error("Attempt to free nullptr");
        return;
    }
    free(*self_ptr);
}

// FIXME: Malamute still lacks that!
// receive message with timeout
static zmsg_t* my_mlm_client_recv(mlm_client_t* client, int timeout)
{
    static zpoller_t* poller = nullptr;

    if (zsys_interrupted)
        return nullptr;

    poller = zpoller_new(mlm_client_msgpipe(client), nullptr);

    zsock_t* which = static_cast<zsock_t*>(zpoller_wait(poller, timeout));
    zpoller_destroy(&poller);
    if (which == mlm_client_msgpipe(client)) {
        zmsg_t* reply = mlm_client_recv(client);
        return reply;
    }
    return nullptr;
}
//  --------------------------------------------------------------------------
//  Publish status of the pointed GPIO sensor

void publish_status(fty_sensor_gpio_server_t* self, gpx_info_t* sensor, int ttl)
{
    log_debug("Publishing GPIO sensor %i (%s) status", sensor->gpx_number, sensor->asset_name);

    zhash_t* aux = zhash_new();
    zhash_autofree(aux);
    char port[6]; // "GPI" + "xx" + '\0'
    memset(&port[0], 0, 6);
    snprintf(&port[0], 6, "GP%c%i", ((sensor->gpx_direction == GPIO_DIRECTION_IN) ? 'I' : 'O'), sensor->gpx_number);
    zhash_insert(aux, FTY_PROTO_METRICS_SENSOR_AUX_PORT, static_cast<void*>(&port[0]));
    zhash_insert(aux, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, static_cast<void*>(sensor->asset_name));
    std::string msg_type = std::string("status.") + &port[0];

    zmsg_t* msg = fty_proto_encode_metric(aux, uint64_t(time(nullptr)), uint32_t(ttl), msg_type.c_str(),
        sensor->parent, // sensor->asset_name
        libgpio_get_status_string(sensor->current_state).c_str(), "");
    zhash_destroy(&aux);
    if (msg) {
        std::string topic = msg_type + std::string("@") + sensor->parent;
        //        "status." + port() + "@" + _location;

        log_debug("\tPort: %s, type: %s, status: %s", &port[0], msg_type.c_str(),
            libgpio_get_status_string(sensor->current_state).c_str());

        int r = mlm_client_send(self->mlm, topic.c_str(), &msg);
        if (r != 0)
            log_debug("failed to send measurement %s result %", topic.c_str(), r);
        zmsg_destroy(&msg);
    }
}

//  --------------------------------------------------------------------------
//  Check GPIO status and generate alarms if needed

static void s_check_gpio_status(fty_sensor_gpio_server_t* self)
{
    pthread_mutex_lock(&gpx_list_mutex);

    // number of sensors monitored in gpx_list
    zlistx_t* gpx_list = get_gpx_list();
    if (!gpx_list) {
        log_debug("GPx list not initialized, skipping");
        pthread_mutex_unlock(&gpx_list_mutex);
        return;
    }
    int          sensors_count = int(zlistx_size(gpx_list));
    gpx_info_t* gpx_info      = nullptr;

    if (sensors_count == 0) {
        log_debug("No sensors monitored");
        pthread_mutex_unlock(&gpx_list_mutex);
        return;
    } else
        log_debug("%i sensor(s) monitored", sensors_count);

    if (!mlm_client_connected(self->mlm)) {
        pthread_mutex_unlock(&gpx_list_mutex);
        return;
    }

    // Acquire the current sensor
    gpx_info = static_cast<gpx_info_t*>(zlistx_first(gpx_list));

    // Loop on all sensors
    for (int cur_sensor_num = 0; cur_sensor_num < sensors_count; cur_sensor_num++) {

        // No processing if not yet init'ed
        if (gpx_info) {

            log_debug("Checking status of GPx sensor '%s'", gpx_info->asset_name);

            // If there is a GPO power source, then activate it prior to
            // accessing the GPI!
            if (gpx_info->power_source && (!streq(gpx_info->power_source, ""))) {
                log_debug("Activating GPO power source %s", gpx_info->power_source);

                if (libgpio_write(self->gpio_lib, atoi(gpx_info->power_source), GPIO_STATE_OPENED) != 0) {
                    log_error("Failed to activate GPO power source!");
                } else {
                    log_debug("GPO power source successfully activated.");
                    // Save the current state
                    gpx_info->current_state = gpx_info->normal_state;
                    // Sleep for a second to have the GPx sensor powered and running
                    zclock_sleep(1000);
                }
            }

            // get the correct GPO status if applicable
            gpo_state_t* state =
                static_cast<gpo_state_t*>(zhashx_lookup(self->gpo_states, static_cast<void*>(gpx_info->asset_name)));
            if ((state && (gpx_info->current_state == GPIO_STATE_UNKNOWN))) {
                gpx_info->current_state = state->last_action;
                log_debug("changed GPO state from GPIO_STATE_UNKNOWN to %s",
                    libgpio_get_status_string(gpx_info->current_state).c_str());
            }

            // Get the current sensor status, only for GPIs, or when no status
            // have been set to GPOs. Otherwise, that reinit GPOs!
            if ((gpx_info->gpx_direction != GPIO_DIRECTION_OUT) || (gpx_info->current_state == GPIO_STATE_UNKNOWN)) {
                gpx_info->current_state = libgpio_read(self->gpio_lib, gpx_info->gpx_number, gpx_info->gpx_direction);
                if (state)
                    state->last_action = gpx_info->current_state;
            }
            if (gpx_info->current_state == GPIO_STATE_UNKNOWN) {
                log_error("Can't read GPx sensor #%i status", gpx_info->gpx_number);
            } else {
                log_debug("Read '%s' (value: %i) on GPx sensor #%i (%s/%s)",
                    libgpio_get_status_string(gpx_info->current_state).c_str(), gpx_info->current_state,
                    gpx_info->gpx_number, gpx_info->ext_name, gpx_info->asset_name);

                publish_status(self, gpx_info, 300);
            }
        }
        gpx_info = static_cast<gpx_info_t*>(zlistx_next(gpx_list));
    }
    pthread_mutex_unlock(&gpx_list_mutex);
}

//  --------------------------------------------------------------------------
//  process message from MAILBOX DELIVER
void static s_handle_mailbox(fty_sensor_gpio_server_t* self, zmsg_t* message)
{
    std::string subject = mlm_client_subject(self->mlm);
    /*    std::string command = zmsg_popstr (message);
        if (command == "") {
            zmsg_destroy (&message);
            log_warning ("Empty subject.");
            return;
        }

        if (command != "GET") {
            log_warning ("%s: Received unexpected command '%s'", self->name, command.c_str());
            zmsg_t *reply = zmsg_new ();
            zmsg_addstr(reply, "ERROR");
            zmsg_addstr (reply, "BAD_COMMAND");
            mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), nullptr, 1000, &reply);
            zmsg_destroy (&reply);
            return;
        }
    */
    // we assume all request command are MAILBOX DELIVER, and subject="gpio"
    if ((subject != "") && (subject != "GPO_INTERACTION") && (subject != "GPIO_TEMPLATE_ADD") &&
        (subject != "GPIO_MANIFEST") && (subject != "GPIO_MANIFEST_SUMMARY") && (subject != "GPIO_TEST") &&
        (subject != "GPOSTATE") && (subject != "ERROR")) {
        log_warning("%s: Received unexpected subject '%s' from '%s'", self->name, subject.c_str(),
            mlm_client_sender(self->mlm));
        zmsg_t* reply = zmsg_new();
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr(reply, "BAD_COMMAND");
        mlm_client_sendto(self->mlm, mlm_client_sender(self->mlm), subject.c_str(), nullptr, 1000, &reply);
        zmsg_destroy(&reply);
        return;
    } else {
        zmsg_t* reply = zmsg_new();
        log_debug("%s: '%s' requested", self->name, subject.c_str());

        if (subject == "GPO_INTERACTION") {
            char* zuuid = zmsg_popstr(message);
            zmsg_addstr(reply, zuuid);
            char* sensor_name = zmsg_popstr(message);
            char* action_name = zmsg_popstr(message);
            log_debug("GPO_INTERACTION: do '%s' on '%s'", action_name, sensor_name);
            // Get the GPO entry for details
            pthread_mutex_lock(&gpx_list_mutex);
            zlistx_t* gpx_list = get_gpx_list();
            if (gpx_list) {
                int          sensors_count = int(zlistx_size(gpx_list));
                gpx_info_t* gpx_info      = static_cast<gpx_info_t*>(zlistx_first(gpx_list));
                gpx_info                   = static_cast<gpx_info_t*>(zlistx_next(gpx_list));
                for (int cur_sensor_num = 0; cur_sensor_num < sensors_count; cur_sensor_num++) {
                    // Check both asset and ext name
                    if (gpx_info && gpx_info->asset_name && gpx_info->ext_name) {
                        log_debug("GPO_INTERACTION: checking sensor %s/%s", gpx_info->asset_name, gpx_info->ext_name);
                        if (streq(gpx_info->asset_name, sensor_name) || streq(gpx_info->ext_name, sensor_name)) {
                            break;
                        }
                    }
                    gpx_info = static_cast<gpx_info_t*>(zlistx_next(gpx_list));
                }
                if ((gpx_info) && (gpx_info->gpx_direction == GPIO_DIRECTION_OUT) &&
                    ((streq(gpx_info->asset_name, sensor_name)) || streq(gpx_info->ext_name, sensor_name))) {
                    int status_value  = libgpio_get_status_value(action_name);
                    int current_state = gpx_info->current_state;

                    if (status_value != GPIO_STATE_UNKNOWN) {
                        // check whether this action is allowed in this state
                        if (status_value == current_state) {
                            log_error("Current state is %s, GPO is requested to become %s",
                                (libgpio_get_status_string(current_state)).c_str(),
                                (libgpio_get_status_string(status_value)).c_str());
                            zmsg_addstr(reply, "ERROR");
                            zmsg_addstr(reply, "ACTION_NOT_APPLICABLE");
                        } else {
                            if (libgpio_write(self->gpio_lib, gpx_info->gpx_number, status_value) != 0) {
                                log_error("GPO_INTERACTION: failed to set value!");
                                zmsg_addstr(reply, "ERROR");
                                zmsg_addstr(reply, "SET_VALUE_FAILED");
                            } else {
                                zmsg_addstr(reply, "OK");
                                // Update the GPO state
                                gpx_info->current_state = status_value;

                                gpo_state_t* last_state =
                                    static_cast<gpo_state_t*>(zhashx_lookup(self->gpo_states, gpx_info->asset_name));
                                if (last_state == nullptr) {
                                    log_debug("GPO_INTERACTION: can't find sensor '%s'!", sensor_name);
                                    zmsg_addstr(reply, "ERROR");
                                    zmsg_addstr(reply, "ASSET_NOT_FOUND");
                                } else {
                                    log_debug(
                                        "last action = %d on port ", last_state->last_action, last_state->gpo_number);
                                    last_state->last_action = status_value;
                                    last_state->in_alert    = 1;
                                }
                            }
                        }
                    } else {
                        log_debug("GPO_INTERACTION: status value is unknown!");
                        zmsg_addstr(reply, "ERROR");
                        zmsg_addstr(reply, "UNKNOWN_VALUE");
                    }
                } else {
                    log_debug("GPO_INTERACTION: can't find sensor '%s'!", sensor_name);
                    zmsg_addstr(reply, "ERROR");
                    zmsg_addstr(reply, "ASSET_NOT_FOUND");
                }
                // send the reply
                int rv =
                    mlm_client_sendto(self->mlm, mlm_client_sender(self->mlm), subject.c_str(), nullptr, 5000, &reply);
                if (rv == -1)
                    log_error("%s:\tgpio: mlm_client_sendto failed", self->name);
            }
            pthread_mutex_unlock(&gpx_list_mutex);
            zstr_free(&sensor_name);
            zstr_free(&action_name);
            zstr_free(&zuuid);
        } else if ((subject == "GPIO_MANIFEST") || (subject == "GPIO_MANIFEST_SUMMARY")) {
            // FIXME: consolidate code using filters
            bool  first = true;
            char* zuuid = zmsg_popstr(message);
            zmsg_addstr(reply, zuuid);
            char* asset_partnumber = zmsg_popstr(message);
            // Check for a parameter, to send (a) specific template(s)
            if (asset_partnumber) {
                while (asset_partnumber) {
                    log_debug("Asset filter provided: %s", asset_partnumber);
                    // FIXME: use @datadir@ (how?)!
                    std::string template_filename =
                        std::string(self->template_dir) + std::string(asset_partnumber) + std::string(".tpl");

                    // We have a GPIO sensor, process it
                    zconfig_t* sensor_template_file = zconfig_load(template_filename.c_str());
                    if (!sensor_template_file) {
                        log_debug("Can't load sensor template file"); // FIXME: error
                        zmsg_addstr(reply, "ERROR");
                        zmsg_addstr(reply, "ASSET_NOT_FOUND");
                        // FIXME: should we break for 1 issue or?
                        break;
                    } else {
                        log_debug("Template file found for %s", asset_partnumber);
                        // Get info from template
                        const char* manufacturer   = s_get(sensor_template_file, "manufacturer", "");
                        const char* type           = s_get(sensor_template_file, "type", "");
                        const char* normal_state   = s_get(sensor_template_file, "normal-state", "");
                        const char* gpx_direction  = s_get(sensor_template_file, "gpx-direction", "");
                        const char* alarm_severity = s_get(sensor_template_file, "alarm-severity", "");
                        const char* alarm_message  = s_get(sensor_template_file, "alarm-message", "");

                        if (first) {
                            zmsg_addstr(reply, "OK");
                            first = false;
                        }
                        zmsg_addstr(reply, asset_partnumber);
                        zmsg_addstr(reply, manufacturer);
                        zmsg_addstr(reply, type);
                        zmsg_addstr(reply, normal_state);
                        zmsg_addstr(reply, gpx_direction);
                        zmsg_addstr(reply, alarm_severity);
                        zmsg_addstr(reply, alarm_message);
                    }

                    // Get the next one, if there is one
                    zconfig_destroy(&sensor_template_file);
                    zstr_free(&asset_partnumber);
                    asset_partnumber = zmsg_popstr(message);
                }
            } else {
                // Send all templates
                assert(self->template_dir);

                zdir_t* dir = zdir_new(self->template_dir, "-");
                if (!dir) {
                    // log_error ("zdir_new (path = '%s', parent = '-') failed.", self->template_dir);
                    return;
                }

                zlist_t* files = zdir_list(dir);
                if (!files) {
                    zdir_destroy(&dir);
                    // log_error ("zdir_list () failed.");
                    return;
                }

                std::regex file_rex(".+\\.tpl");

                zfile_t* item = static_cast<zfile_t*>(zlist_first(files));
                if (item)
                    zmsg_addstr(reply, "OK");
                while (item) {
                    if (std::regex_match(zfile_filename(item, self->template_dir), file_rex)) {
                        log_debug("%s matched", zfile_filename(item, self->template_dir));
                        std::string template_filename = zfile_filename(item, nullptr);

                        std::string asset_partnumber_str = zfile_filename(item, self->template_dir);
                        asset_partnumber_str.erase(asset_partnumber_str.size() - 4);

                        // We have a GPIO sensor, process it
                        zconfig_t* sensor_template_file = zconfig_load(template_filename.c_str());

                        // Get info from template
                        const char* manufacturer     = s_get(sensor_template_file, "manufacturer", "");
                        const char* type             = s_get(sensor_template_file, "type", "");
                        const char* normal_state     = s_get(sensor_template_file, "normal-state", "");
                        const char* gpx_direction    = s_get(sensor_template_file, "gpx-direction", "");
                        const char* gpx_power_source = s_get(sensor_template_file, "power-source", "");
                        const char* alarm_severity   = s_get(sensor_template_file, "alarm-severity", "");
                        const char* alarm_message    = s_get(sensor_template_file, "alarm-message", "");

                        zmsg_addstr(reply, asset_partnumber_str.c_str());
                        zmsg_addstr(reply, manufacturer);
                        if (subject == "GPIO_MANIFEST") {
                            zmsg_addstr(reply, type);
                            zmsg_addstr(reply, normal_state);
                            zmsg_addstr(reply, gpx_direction);
                            zmsg_addstr(reply, gpx_power_source);
                            zmsg_addstr(reply, alarm_severity);
                            zmsg_addstr(reply, alarm_message);
                        }
                        zconfig_destroy(&sensor_template_file);
                    }
                    item = static_cast<zfile_t*>(zlist_next(files));
                }
                zlist_destroy(&files);
                zdir_destroy(&dir);
            }
            // send the reply
            int rv = mlm_client_sendto(self->mlm, mlm_client_sender(self->mlm), subject.c_str(), nullptr, 5000, &reply);
            if (rv == -1)
                log_error("%s:\tgpio: mlm_client_sendto failed", self->name);
            zstr_free(&zuuid);
        } else if (subject == "GPIO_TEMPLATE_ADD") {
            char* zuuid = zmsg_popstr(message);
            zmsg_addstr(reply, zuuid);
            char* sensor_partnumber = zmsg_popstr(message);
            if (sensor_partnumber) {
                zconfig_t*  root = zconfig_new("root", nullptr);
                std::string template_filename =
                    std::string(self->template_dir) + std::string(sensor_partnumber) + std::string(".tpl");

                // We have a GPIO sensor template, process it
                char*       manufacturer     = zmsg_popstr(message);
                char*       type             = zmsg_popstr(message);
                char*       normal_state     = zmsg_popstr(message);
                char*       gpx_direction    = zmsg_popstr(message);
                char*       gpx_power_source = zmsg_popstr(message);
                char*       alarm_severity   = zmsg_popstr(message);
                std::string alarm_message;
                char*       alarm_message_part;
                // Process the rest of the message as the alarm message
                while (zmsg_size(message)) {
                    alarm_message_part = zmsg_popstr(message);
                    if (!alarm_message.empty())
                        alarm_message += " ";
                    alarm_message += alarm_message_part;
                    zstr_free(&alarm_message_part);
                }

                // Sanity check
                if (!type || alarm_message.empty()) {
                    zmsg_addstr(reply, "ERROR");
                    zmsg_addstr(reply, "MISSING_PARAM");
                } else {
                    // Fill possible missing values with sane defaults
                    if (!manufacturer)
                        manufacturer = strdup("unknown");
                    if (!normal_state)
                        normal_state = strdup("opened");
                    if (!gpx_direction)
                        gpx_direction = strdup("GPI");
                    if (!alarm_severity)
                        alarm_severity = strdup("WARNING");
                }

                zconfig_set_comment(root, " Generated through 42ITy web UI");
                zconfig_put(root, "manufacturer", manufacturer);
                zconfig_put(root, "part-number", sensor_partnumber);
                zconfig_put(root, "type", type);
                zconfig_put(root, "normal-state", normal_state);
                zconfig_put(root, "gpx-direction", gpx_direction);
                zconfig_put(root, "power-source", gpx_power_source);
                zconfig_put(root, "alarm-severity", alarm_severity);
                zconfig_put(root, "alarm-message", alarm_message.c_str());

                // Save the template
                int rv = zconfig_save(root, template_filename.c_str());
                zconfig_destroy(&root);

                // Prepare our answer
                if (rv == 0)
                    zmsg_addstr(reply, "OK");
                else {
                    zmsg_addstr(reply, "ERROR");
                    zmsg_addstr(reply, "UNKNOWN"); // FIXME: check errno
                }
                // Cleanup
                zstr_free(&manufacturer);
                zstr_free(&type);
                zstr_free(&normal_state);
                zstr_free(&gpx_direction);
                zstr_free(&gpx_power_source);
                zstr_free(&alarm_severity);
            } else {
                zmsg_addstr(reply, "ERROR");
                zmsg_addstr(reply, "MISSING_PARAM");
            }
            // send the reply
            int rv = mlm_client_sendto(self->mlm, mlm_client_sender(self->mlm), subject.c_str(), nullptr, 5000, &reply);
            if (rv == -1)
                log_error("%s:\tgpio: mlm_client_sendto failed", self->name);

            zstr_free(&sensor_partnumber);
            zstr_free(&zuuid);
        } else if (subject == "GPOSTATE") {
            // we won't reply
            zmsg_destroy(&reply);

            char* assetname  = zmsg_popstr(message);
            char* gpo_number = zmsg_popstr(message);

            int num_gpo_number = atoi(gpo_number);
            // this means DELETE
            if (num_gpo_number == -1) {
                zhashx_delete(self->gpo_states, static_cast<void*>(assetname));
                zstr_free(&assetname);
                zstr_free(&gpo_number);
                return;
            }

            char* default_state = zmsg_popstr(message);

            gpo_state_t* state =
                static_cast<gpo_state_t*>(zhashx_lookup(self->gpo_states, static_cast<void*>(assetname)));
            if (state != nullptr) {
                int num_default_state = libgpio_get_status_value(default_state);
                // did the default state changed?
                if (state->default_state != num_default_state) {
                    state->default_state = num_default_state;
                    if (!state->in_alert) {
                        int rv = libgpio_write(self->gpio_lib, state->gpo_number, num_default_state);
                        if (rv) {
                            log_error("Error during default action %s on GPO #%d", default_state, state->gpo_number);
                        }
                        state->last_action = num_default_state;
                    }
                }
                // did the port change?
                if (state->gpo_number != num_gpo_number) {
                    // turn off the previous port
                    int rv = libgpio_write(self->gpio_lib, state->gpo_number, GPIO_STATE_CLOSED);
                    if (rv)
                        log_error("Error while closing no longer active GPO #%d", state->gpo_number);

                    // do the default action on the new port
                    num_default_state = libgpio_get_status_value(default_state);
                    rv                = libgpio_write(self->gpio_lib, num_gpo_number, num_default_state);
                    if (rv) {
                        log_error("Error during default action %s on GPO #%d", default_state, state->gpo_number);
                    }
                    state->gpo_number  = num_gpo_number;
                    state->last_action = num_default_state;
                    state->in_alert    = 0;
                }
            } else {
                state                = static_cast<gpo_state_t*>(zmalloc(sizeof(gpo_state_t)));
                state->gpo_number    = atoi(gpo_number);
                state->default_state = libgpio_get_status_value(default_state);
                // do the default action
                int rv = libgpio_write(self->gpio_lib, state->gpo_number, state->default_state);
                if (rv) {
                    log_error("Error during default action %s on GPO #%d", default_state, state->gpo_number);
                    state->last_action = GPIO_STATE_UNKNOWN;
                } else
                    state->last_action = libgpio_get_status_value(default_state);
                state->in_alert = 0;
                zhashx_update(self->gpo_states, static_cast<void*>(assetname), static_cast<void*>(state));
            }

            zstr_free(&assetname);
            zstr_free(&gpo_number);
            zstr_free(&default_state);
        }

        else if (subject == "GPIO_TEST") {
            ;
        }

        else if (subject == "ERROR") {
            // Don't reply to ERROR messages
            log_warning("%s: Received ERROR subject from '%s', ignoring", self->name, mlm_client_sender(self->mlm));
        }
        zmsg_destroy(&reply);
    }
}

//  --------------------------------------------------------------------------
//  Create a new fty_sensor_gpio_server

fty_sensor_gpio_server_t* fty_sensor_gpio_server_new(const char* name)
{
    fty_sensor_gpio_server_t* self = static_cast<fty_sensor_gpio_server_t*>(zmalloc(sizeof(fty_sensor_gpio_server_t)));
    assert(self);

    //  Initialize class properties
    self->mlm          = mlm_client_new();
    self->name         = strdup(name);
    self->test_mode    = false;
    self->template_dir = nullptr;
    // FIXME: we should share access to libgpio for both -server and -asset
    // for the sanity checks on count/offset/...
    self->gpio_lib = libgpio_new();
    assert(self->gpio_lib);
    self->gpo_states = zhashx_new();
    zhashx_set_destructor(self->gpo_states, free_fn);
    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the fty_sensor_gpio_server

void fty_sensor_gpio_server_destroy(fty_sensor_gpio_server_t** self_p)
{
    assert(self_p);
    if (*self_p) {
        fty_sensor_gpio_server_t* self = *self_p;

        //  Free class properties
        libgpio_destroy(&self->gpio_lib);
        zstr_free(&self->name);
        mlm_client_destroy(&self->mlm);
        if (self->template_dir)
            zstr_free(&self->template_dir);
        zhashx_destroy(&self->gpo_states);
        //  Free object itself
        free(self);
        *self_p = nullptr;
    }
}

static void s_load_state_file(fty_sensor_gpio_server_t* self, const char* state_file)
{
    if (!state_file)
        // no state file - alright
        return;
    log_debug("state file = %s", state_file);
    FILE* f_state = fopen(state_file, "r");
    if (!f_state) {
        log_warning("Could not load state file, continuing without it...");
        return;
    }
    char asset_name[15]; // gpo-[0-9]{10} + terminator, which should be enough for DB UINT
    int  gpo_number    = -1;
    int  default_state = -1;
    int  last_action   = -1;
    // line read successfully - all 4 items are there
    while (fscanf(f_state, "%14s %3d %d %d", asset_name, &gpo_number, &default_state, &last_action) == 4) {
        // existing GPO entry came from fty-sensor-gpio-assets, which takes precendence
        gpo_state_t* state = static_cast<gpo_state_t*>(zhashx_lookup(self->gpo_states, static_cast<void*>(asset_name)));

        if (state != nullptr) {
            // did the port change?
            if (state->gpo_number != gpo_number) {
                // turn off the port from state file
                int rv = libgpio_write(self->gpio_lib, gpo_number, GPIO_STATE_CLOSED);
                if (rv)
                    log_error("Error while closing no longer active GPO #%d", state->gpo_number);
                // default action on the new port was done when adding it
            }
        } else {
            state                = static_cast<gpo_state_t*>(zmalloc(sizeof(gpo_state_t)));
            state->gpo_number    = gpo_number;
            state->default_state = default_state;
            // do the default action
            int rv = libgpio_write(self->gpio_lib, state->gpo_number, state->default_state);
            if (rv) {
                log_error("Error during default action %s on GPO #%d", libgpio_get_status_string(default_state).c_str(),
                    state->gpo_number);
                state->last_action = GPIO_STATE_UNKNOWN;
            } else
                state->last_action = default_state;
            state->in_alert = 0;

            char* asset_name_key = strdup(asset_name);
            zhashx_update(self->gpo_states, static_cast<void*>(asset_name_key), static_cast<void*>(state));
        }
    }

    fclose(f_state);
}

static void s_save_state_file(fty_sensor_gpio_server_t* self, const char* state_file)
{
    FILE* f_state = fopen(state_file, "w");

    gpo_state_t* state = static_cast<gpo_state_t*>(zhashx_first(self->gpo_states));
    while (state != nullptr) {
        const char* asset_name = static_cast<const char*>(zhashx_cursor(self->gpo_states));
        fprintf(f_state, "%s %d %d %d\n", asset_name, state->gpo_number, state->default_state, state->last_action);
        state = static_cast<gpo_state_t*>(zhashx_next(self->gpo_states));
    }

    fclose(f_state);
}

//  --------------------------------------------------------------------------
//  Request GPI/GPO capabilities from fty-info, to init our structures.
//  Return 1 on error, 0 otherwise
int request_capabilities_info(fty_sensor_gpio_server_t* self, const char* type)
{
    log_debug("%s:\tRequest GPIO capabilities info for '%s'", self->name, type);

    // Sanity check
    if ((!streq(type, "gpi")) && (!streq(type, "gpo"))) {
        log_error("only 'gpi' and 'gpo' are supported");
        return 1;
    }

    zmsg_t* reply;
    if (!self->test_mode) {
        // Request HW_CAP info for <type>
        zmsg_t*  msg  = zmsg_new();
        zuuid_t* uuid = zuuid_new();
        zmsg_addstr(msg, "HW_CAP");
        zmsg_addstr(msg, zuuid_str_canonical(uuid));
        zmsg_addstr(msg, type);

        int rv = mlm_client_sendto(self->mlm, "fty-info", "info", nullptr, 5000, &msg);
        if (rv != 0) {
            log_error("%s:\tRequest %s sensors list failed", self->name, type);
            zmsg_destroy(&msg);
            zuuid_destroy(&uuid);
            return 1;
        } else
            log_debug("%s: %s capability request sent successfully", self->name, type);

        reply = my_mlm_client_recv(self->mlm, 5000);
        if (!reply) {
            log_error("%s: no reply message received", self->name);
            return 1;
        }

        char* uuid_recv = zmsg_popstr(reply);

        if (0 != strcmp(zuuid_str_canonical(uuid), uuid_recv)) {
            log_debug("%s: zuuid reply doesn't match request", self->name);
            zmsg_destroy(&reply);
            zstr_free(&uuid_recv);
            return 1;
        }

        if (streq(zmsg_popstr(reply), "ERROR")) {
            char* reason = zmsg_popstr(reply);
            log_error("%s: error message received %s", self->name, reason);
            return 1;
        }
    } else { // TEST mode
        // Use the forged reply
        if (streq(type, "gpi")) {
            reply = hw_cap_test_reply_gpi;
        } else if (streq(type, "gpo")) {
            reply = hw_cap_test_reply_gpo;
        } else {
            reply = nullptr;
        }
    }

    // sanity check on type requested Vs received
    char* value = zmsg_popstr(reply);
    if (!streq(value, type)) {
        log_error("%s: mismatch in reply on the type received (should be %s ; is %s)", self->name, type, value);
        zstr_free(&value);
        return 1;
    }
    zstr_free(&value);

    // Process the GPx count
    value      = zmsg_popstr(reply);
    int ivalue = atoi(value);
    log_debug("%s count=%i", type, ivalue);
    if (streq(type, "gpi")) {
        libgpio_set_gpi_count(self->gpio_lib, ivalue);
    } else if (streq(type, "gpo")) {
        libgpio_set_gpo_count(self->gpio_lib, ivalue);
    }
    zstr_free(&value);

    if (ivalue == 0) {
        log_debug("%s count is 0, no further processing", type);
        return 0;
    }

    // Process the GPIO chipset base address
    value  = zmsg_popstr(reply);
    ivalue = atoi(value);
    log_debug("%s chipset base address: %i", type, ivalue);
    libgpio_set_gpio_base_address(self->gpio_lib, ivalue);
    zstr_free(&value);

    // Process the offset of the GPI/O
    value  = zmsg_popstr(reply);
    ivalue = atoi(value);
    log_debug("%s offset=%i", type, ivalue);
    if (streq(type, "gpi")) {
        libgpio_set_gpi_offset(self->gpio_lib, ivalue);
    } else if (streq(type, "gpo")) {
        libgpio_set_gpo_offset(self->gpio_lib, ivalue);
    }
    zstr_free(&value);

    // Process port mapping
    value = zmsg_popstr(reply);
    while (value) {
        // GPx pin name
        // drop the port descriptor because zconfig is stupid and
        // doesn't allow number as a key
        const std::string port_str(value + 1, 1);
        // convert to int
        int port_num = static_cast<int>(strtol(port_str.c_str(), nullptr, 10));
        zstr_free(&value);
        // GPx pin number
        value       = zmsg_popstr(reply);
        int pin_num = static_cast<int>(strtol(value, nullptr, 10));
        if (streq(type, "gpi"))
            libgpio_add_gpi_mapping(self->gpio_lib, port_num, pin_num);
        else
            libgpio_add_gpo_mapping(self->gpio_lib, port_num, pin_num);
        zstr_free(&value);
        // Pop the next pin name
        value = zmsg_popstr(reply);
    }
    zmsg_destroy(&reply);
    return 0;
}

//  --------------------------------------------------------------------------
//  Create fty_sensor_gpio_server actor

void fty_sensor_gpio_server(zsock_t* pipe, void* args)
{
    char* name = static_cast<char*>(args);
    if (!name) {
        log_error("Adress for fty-sensor-gpio actor is nullptr");
        return;
    }
    char* state_file_path = nullptr;

    fty_sensor_gpio_server_t* self = fty_sensor_gpio_server_new(name);
    assert(self);

    zpoller_t* poller = zpoller_new(pipe, mlm_client_msgpipe(self->mlm), nullptr);
    assert(poller);

    zsock_signal(pipe, 0);
    log_info("%s_server: Started", self->name);

    while (!zsys_interrupted) {
        void* which = zpoller_wait(poller, TIMEOUT_MS);
        if (which == nullptr) {
            if (zpoller_terminated(poller) || zsys_interrupted) {
                break;
            }
        }
        if (which == pipe) {
            zmsg_t* message = zmsg_recv(pipe);
            char*   cmd     = zmsg_popstr(message);
            if (cmd) {
                log_trace("received command %s", cmd);
                if (streq(cmd, "$TERM")) {
                    zstr_free(&cmd);
                    zmsg_destroy(&message);
                    goto exit;
                } else if (streq(cmd, "CONNECT")) {
                    char* endpoint = zmsg_popstr(message);
                    if (!endpoint)
                        log_error("%s:\tMissing endpoint", self->name);
                    assert(endpoint);
                    int r = mlm_client_connect(self->mlm, endpoint, 5000, self->name);
                    if (r == -1)
                        log_error("%s:\tConnection to endpoint '%s' failed", self->name, endpoint);
                    log_debug("CONNECT %s/%s", endpoint, self->name);
                    zstr_free(&endpoint);
                } else if (streq(cmd, "PRODUCER")) {
                    char* stream = zmsg_popstr(message);
                    assert(stream);
                    mlm_client_set_producer(self->mlm, stream);
                    log_debug("fty_sensor_gpio: setting PRODUCER on %s", stream);
                    zstr_free(&stream);
                } else if (streq(cmd, "CONSUMER")) {
                    char* stream  = zmsg_popstr(message);
                    char* pattern = zmsg_popstr(message);
                    assert(stream && pattern);
                    mlm_client_set_consumer(self->mlm, stream, pattern);
                    log_debug("fty_sensor_gpio: setting CONSUMER on %s/%s", stream, pattern);
                    zstr_free(&stream);
                    zstr_free(&pattern);
                } else if (streq(cmd, "TEST")) {
                    self->test_mode = true;
                    libgpio_set_test_mode(self->gpio_lib, self->test_mode);
                    log_debug("fty_sensor_gpio: TEST=true");
                } else if (streq(cmd, "UPDATE")) {
                    s_check_gpio_status(self);
                } else if (streq(cmd, "TEMPLATE_DIR")) {
                    self->template_dir = zmsg_popstr(message);
                    log_debug("fty_sensor_gpio: Using sensors template directory: %s", self->template_dir);
                } else if (streq(cmd, "HW_CAP")) {
                    // Request our config
                    int rvi = request_capabilities_info(self, "gpi");
                    int rvo = request_capabilities_info(self, "gpo");
                    // We can now stop the reschedule loop
                    if (!rvi && !rvo) {
                        log_debug("HW_CAP request succeeded");
                        hw_cap_inited = true;
                    }
                } else if (streq(cmd, "STATEFILE")) {
                    char* state_file = zmsg_popstr(message);
                    s_load_state_file(self, state_file);
                    state_file_path = state_file;
                } else {
                    log_warning("\tUnknown API command=%s, ignoring", cmd);
                }
                zstr_free(&cmd);
            }
            zmsg_destroy(&message);
        } else if (which == mlm_client_msgpipe(self->mlm)) {
            zmsg_t* message = mlm_client_recv(self->mlm);
            if (streq(mlm_client_command(self->mlm), "MAILBOX DELIVER")) {
                // someone is addressing us directly
                s_handle_mailbox(self, message);
            }
            zmsg_destroy(&message);
        }
    }
exit:
    if (!self->test_mode)
        s_save_state_file(self, state_file_path);
    zstr_free(&state_file_path);
    zpoller_destroy(&poller);
    fty_sensor_gpio_server_destroy(&self);
}
