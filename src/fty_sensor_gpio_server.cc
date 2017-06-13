/*  =========================================================================
    fty_sensor_gpio_server - Actor

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
    fty_sensor_gpio_server - Actor
@discuss
     GPIO PROTOCOL

     ------------------------------------------------------------------------
    ## GPO_INTERACTION

    REQ:
        subject: "GPO_INTERACTION"
        Message is a multipart string message

        /sensor/action              - apply action (open | close) on sensor (asset or ext name)
                                      beside from open and close, opened | high and closed | low
                                      are also supported

    REP:
        subject: "GPO_INTERACTION"
        Message is a multipart message:

        * OK                         = action applied successfully
        * ERROR/<reason>

        where:
            <reason>          = ASSET_NOT_FOUND / SET_VALUE_FAILED / UNKNOWN_VALUE / BAD_COMMAND

     ------------------------------------------------------------------------
    ## GPIO_MANIFEST

    REQ:
        subject: "GPIO_MANIFEST"
        Message is a multipart string message

        /<sensor 1 part number>/.../<sensor N part number>      - get information on sensor(s)

        where:
            <sensor x part number>   = Part number of the sensor(s), to get information on
                                       when empty, return all supported sensors information

    REP:
        subject: "GPIO_MANIFEST"
        Message is a multipart message:

        * OK/<sensor 1 description>/.../<sensor N description> = non-empty
        * ERROR/<reason>

        where:
            <reason>                 = ASSET_NOT_FOUND / BAD_COMMAND
            <sensor N description> = sensor_partnumber/manufacturer/type/normal_state/gpx_direction/alarm_severity/alarm_message

     ------------------------------------------------------------------------
    ## GPIO_MANIFEST_SUMMARY

    REQ:
        subject: "GPIO_MANIFEST_SUMMARY"
        Message is a multipart string message

              - get the list of supported sensors
                this is a light version of GPIO_MANIFEST, only returning
                sensor_partnumber/manufacturer

    REP:
        subject: "GPIO_MANIFEST_SUMMARY"
        Message is a multipart message:

        * OK/<sensor 1 description>/.../<sensor N description> = non-empty
        * ERROR/<reason>

        where:
            <reason>                 = ASSET_NOT_FOUND / BAD_COMMAND
            <sensor N description> = sensor_partnumber/manufacturer

     ------------------------------------------------------------------------
    ## GPIO_TEMPLATE_ADD

    REQ:
        subject: "GPIO_TEMPLATE_ADD"
        Message is a multipart string message

        /<sensor description>      - request the creation of a sensor template file

        where:
            <sensor description> = sensor_partnumber/manufacturer/type/normal_state/gpx_direction/alarm_severity/alarm_message

    REP:
        subject: "GPIO_TEMPLATE_ADD"
        Message is a multipart message:

        * OK
        * ERROR/<reason>

        where:
            <reason>             = ...


@end
*/

#include "fty_sensor_gpio_classes.h"
#include <regex>

//  Structure of our class

struct _fty_sensor_gpio_server_t {
    bool               verbose;       // is actor verbose or not
    char               *name;         // actor name
    mlm_client_t       *mlm;          // malamute client
    libgpio_t          *gpio_lib;     // GPIO library handle
    bool               test_mode;     // true if we are in test mode, false otherwise
    char               *template_dir; // Location of the template files
};

// Configuration accessors
// FIXME: why do we need that? zconfig_get should already do this, no?
const char*
s_get (zconfig_t *config, const char* key, std::string &dfl) {
    assert (config);
    char *ret = zconfig_get (config, key, dfl.c_str());
    if (!ret || streq (ret, ""))
        return (char*)dfl.c_str();
    return ret;
}

const char*
s_get (zconfig_t *config, const char* key, const char*dfl) {
    assert (config);
    char *ret = zconfig_get (config, key, dfl);
    if (!ret || streq (ret, ""))
        return dfl;
    return ret;
}

//  --------------------------------------------------------------------------
//  Publish status of the pointed GPIO sensor

void publish_status (fty_sensor_gpio_server_t *self, _gpx_info_t *sensor, int ttl)
{
    my_zsys_debug(self->verbose, "Publishing GPIO sensor %i (%s) status",
        sensor->gpx_number, sensor->asset_name);

        zhash_t *aux = zhash_new ();
        zhash_autofree (aux);
        char port[6];  // "GPI" + "xx" + '\0'
        memset(&port[0], 0, 6);
        snprintf(&port[0], 6, "GPI%i", sensor->gpx_number);
        zhash_insert (aux, "port", (void*) &port[0]);
        string msg_type = string("status.") + &port[0];

        zmsg_t *msg = fty_proto_encode_metric (
            aux,
            time (NULL),
            ttl,
            msg_type.c_str (),
            sensor->asset_name, //sensor->location,
            libgpio_get_status_string(sensor->current_state).c_str(),
            "");
        zhash_destroy (&aux);
        if (msg) {
            std::string topic = msg_type + string("@") + sensor->asset_name; //sensor->location;

            my_zsys_debug(self->verbose, "\tPort: %s, type: %s, status: %s",
                &port[0], msg_type.c_str(),
                libgpio_get_status_string(sensor->current_state).c_str());

            int r = mlm_client_send (self->mlm, topic.c_str (), &msg);
            if( r != 0 )
                my_zsys_debug(self->verbose, "failed to send measurement %s result %", topic.c_str(), r);
            zmsg_destroy (&msg);
        }
}

//  --------------------------------------------------------------------------
//  Check GPIO status and generate alarms if needed

static void
s_check_gpio_status(fty_sensor_gpio_server_t *self)
{
    my_zsys_debug (self->verbose, "%s_server: %s", self->name, __func__);

    pthread_mutex_lock (&gpx_list_mutex);

    // number of sensors monitored in gpx_list
    zlistx_t *gpx_list = get_gpx_list(self->verbose);
    if (!gpx_list) {
        my_zsys_debug (self->verbose, "GPx list not initialized, skipping");
        pthread_mutex_unlock (&gpx_list_mutex);
        return;
    }
    int sensors_count = zlistx_size (gpx_list);
    _gpx_info_t *gpx_info = NULL;

    if (sensors_count == 0) {
        my_zsys_debug (self->verbose, "No sensors monitored");
        pthread_mutex_unlock (&gpx_list_mutex);
        return;
    }
    else
        my_zsys_debug (self->verbose, "%i sensor(s) monitored", sensors_count);

    if(!mlm_client_connected(self->mlm)) {
        pthread_mutex_unlock (&gpx_list_mutex);
        return;
    }

    // Acquire the current sensor
    gpx_info = (_gpx_info_t *)zlistx_first (gpx_list);

    // Loop on all sensors
    for (int cur_sensor_num = 0; cur_sensor_num < sensors_count; cur_sensor_num++) {

        // No processing if not yet init'ed
        if (gpx_info) {
            // Get the current sensor status
            gpx_info->current_state = libgpio_read( self->gpio_lib,
                                                    gpx_info->gpx_number,
                                                    gpx_info->gpx_direction);

            if (gpx_info->current_state == GPIO_STATE_UNKNOWN) {
                my_zsys_debug (self->verbose, "Can't read GPI sensor #%i status", gpx_info->gpx_number);
            }
            else {
                my_zsys_debug (self->verbose, "Read '%s' (value: %i) on GPx sensor #%i (%s/%s)",
                    libgpio_get_status_string(gpx_info->current_state).c_str(),
                    gpx_info->current_state, gpx_info->gpx_number,
                    gpx_info->ext_name, gpx_info->asset_name);

                publish_status (self, gpx_info, 300);
            }
        }
        gpx_info = (_gpx_info_t *)zlistx_next (gpx_list);
    }
    pthread_mutex_unlock (&gpx_list_mutex);
}

//  --------------------------------------------------------------------------
//  process message from MAILBOX DELIVER
void static
s_handle_mailbox(fty_sensor_gpio_server_t* self, zmsg_t *message)
{
    std::string subject = mlm_client_subject (self->mlm);
/*    std::string command = zmsg_popstr (message);
    if (command == "") {
        zmsg_destroy (&message);
        zsys_warning ("Empty subject.");
        return;
    }

    if (command != "GET") {
        zsys_warning ("%s: Received unexpected command '%s'", self->name, command.c_str());
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr (reply, "BAD_COMMAND");
        mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), NULL, 1000, &reply);
        zmsg_destroy (&reply);
        return;
    }
*/
    //we assume all request command are MAILBOX DELIVER, and subject="gpio"
    if ( (subject != "") && (subject != "GPO_INTERACTION") && (subject != "GPIO_TEMPLATE_ADD")
         && (subject != "GPIO_MANIFEST") && (subject != "GPIO_MANIFEST_SUMMARY")
         && (subject != "GPIO_TEST")) {
        zsys_warning ("%s: Received unexpected subject '%s'", self->name, subject.c_str());
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr (reply, "BAD_COMMAND");
        mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), NULL, 1000, &reply);
        zmsg_destroy (&reply);
        return;
    }
    else {
        zmsg_t *reply = zmsg_new ();
        my_zsys_debug (self->verbose, "%s: '%s' requested", self->name, subject.c_str());

        if (subject == "GPO_INTERACTION") {
            char *sensor_name = zmsg_popstr (message);
            char *action_name = zmsg_popstr (message);
            my_zsys_debug (self->verbose, "GPO_INTERACTION: do '%s' on '%s'",
                action_name, sensor_name);
            // Get the GPO entry for details
            pthread_mutex_lock (&gpx_list_mutex);
            zlistx_t *gpx_list = get_gpx_list(self->verbose);
            if (gpx_list) {
                int sensors_count = zlistx_size (gpx_list);
                _gpx_info_t *gpx_info = (_gpx_info_t *)zlistx_first (gpx_list);
                gpx_info = (_gpx_info_t *)zlistx_next (gpx_list);
                for (int cur_sensor_num = 0; cur_sensor_num < sensors_count; cur_sensor_num++) {
                    // Check both asset and ext name
                    if (gpx_info && gpx_info->asset_name && gpx_info->ext_name) {
                        my_zsys_debug (self->verbose, "GPO_INTERACTION: checking sensor %s/%s",
                            gpx_info->asset_name, gpx_info->ext_name);
                        if ( streq(gpx_info->asset_name, sensor_name)
                            || streq(gpx_info->ext_name, sensor_name) ) {
                                break;
                        }
                    }
                    gpx_info = (_gpx_info_t *)zlistx_next (gpx_list);
                }
                if ( (gpx_info) && ((streq(gpx_info->asset_name, sensor_name))
                    || streq(gpx_info->ext_name, sensor_name)) ) {
                    int status_value = libgpio_get_status_value (action_name);

                    if (status_value != GPIO_STATE_UNKNOWN) {
                        if (libgpio_write (self->gpio_lib, gpx_info->gpx_number, status_value) != 0) {
                            my_zsys_debug (self->verbose, "GPO_INTERACTION: failed to set value!");
                            zmsg_addstr (reply, "ERROR");
                            zmsg_addstr (reply, "SET_VALUE_FAILED");
                        }
                        else {
                            zmsg_addstr (reply, "OK");
                        }
                    }
                    else {
                        my_zsys_debug (self->verbose, "GPO_INTERACTION: status value is unknown!");
                        zmsg_addstr (reply, "ERROR");
                        zmsg_addstr (reply, "UNKNOWN_VALUE");
                    }
                }
                else {
                    my_zsys_debug (self->verbose, "GPO_INTERACTION: can't find sensor '%s'!", sensor_name);
                    zmsg_addstr (reply, "ERROR");
                    zmsg_addstr (reply, "ASSET_NOT_FOUND");
                }
                // send the reply
                int rv = mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), NULL, 5000, &reply);
                if (rv == -1)
                    zsys_error ("%s:\tgpio: mlm_client_sendto failed", self->name);
            }
            pthread_mutex_unlock (&gpx_list_mutex);
            zstr_free(&sensor_name);
            zstr_free(&action_name);
        }
        else if ( (subject == "GPIO_MANIFEST") || (subject == "GPIO_MANIFEST_SUMMARY") ) {
            // FIXME: consolidate code using filters
            zmsg_t *reply = zmsg_new ();
            zmsg_addstr (reply, "OK");
            char *asset_partnumber = zmsg_popstr (message);
            // Check for a parameter, to send (a) specific template(s)
            if (asset_partnumber) {
                while (asset_partnumber) {
                    // FIXME: use @datadir@ (how?)!
                    string template_filename = string(self->template_dir) + string(asset_partnumber) + string(".tpl");

                    // We have a GPIO sensor, process it
                    zconfig_t *sensor_template_file = zconfig_load (template_filename.c_str());
                    if (!sensor_template_file) {
                        my_zsys_debug (self->verbose, "Can't load sensor template file"); // FIXME: error
                        zmsg_addstr (reply, "ERROR");
                        zmsg_addstr (reply, "ASSET_NOT_FOUND");
                        // FIXME: should we break for 1 issue or?
                        break;
                    }
                    else {
                        my_zsys_debug (self->verbose, "Template file found for %s", asset_partnumber);
                        // Get info from template
                        const char *manufacturer = s_get (sensor_template_file, "manufacturer", "");
                        const char *type = s_get (sensor_template_file, "type", "");
                        const char *normal_state = s_get (sensor_template_file, "normal-state", "");
                        const char *gpx_direction = s_get (sensor_template_file, "gpx-direction", "");
                        const char *alarm_severity = s_get (sensor_template_file, "alarm-severity", "");
                        const char *alarm_message = s_get (sensor_template_file, "alarm-message", "");

                        zmsg_addstr (reply, asset_partnumber);
                        zmsg_addstr (reply, manufacturer);
                        zmsg_addstr (reply, type);
                        zmsg_addstr (reply, normal_state);
                        zmsg_addstr (reply, gpx_direction);
                        zmsg_addstr (reply, alarm_severity);
                        zmsg_addstr (reply, alarm_message);
                    }

                    // Get the next one, if there is one
                    zconfig_destroy (&sensor_template_file);                    
                    zstr_free(&asset_partnumber);
                    asset_partnumber = zmsg_popstr (message);
                }
            }
            else {
                // Send all templates
                assert (self->template_dir);

                zdir_t *dir = zdir_new (self->template_dir, "-");
                if (!dir) {
                    //log_error ("zdir_new (path = '%s', parent = '-') failed.", self->template_dir);
                    return;
                }

                zlist_t *files = zdir_list (dir);
                if (!files) {
                    zdir_destroy (&dir);
                    //log_error ("zdir_list () failed.");
                    return;
                }

                std::regex file_rex (".+\\.tpl");

                zfile_t *item = (zfile_t *) zlist_first (files);
                while (item) {
                    if (std::regex_match (zfile_filename (item, self->template_dir), file_rex)) {
                        my_zsys_debug (self->verbose, "%s: %s matched", __func__, zfile_filename (item, self->template_dir));
                        string template_filename = zfile_filename (item, NULL);
                        
                        string asset_partnumber = zfile_filename (item, self->template_dir);
                        asset_partnumber.erase (asset_partnumber.size () - 4);

                        // We have a GPIO sensor, process it
                        zconfig_t *sensor_template_file = zconfig_load (template_filename.c_str());

                        // Get info from template
                        const char *manufacturer = s_get (sensor_template_file, "manufacturer", "");
                        const char *type = s_get (sensor_template_file, "type", "");
                        const char *normal_state = s_get (sensor_template_file, "normal-state", "");
                        const char *gpx_direction = s_get (sensor_template_file, "gpx-direction", "");
                        const char *alarm_severity = s_get (sensor_template_file, "alarm-severity", "");
                        const char *alarm_message = s_get (sensor_template_file, "alarm-message", "");

                        zmsg_addstr (reply, asset_partnumber.c_str());
                        zmsg_addstr (reply, manufacturer);
                        if (subject == "GPIO_MANIFEST") {
                            zmsg_addstr (reply, type);
                            zmsg_addstr (reply, normal_state);
                            zmsg_addstr (reply, gpx_direction);
                            zmsg_addstr (reply, alarm_severity);
                            zmsg_addstr (reply, alarm_message);
                        }
                        zconfig_destroy (&sensor_template_file);
                    }
                    item = (zfile_t *) zlist_next (files);
                }
                zlist_destroy (&files);
                zdir_destroy (&dir);
            }
            // send the reply
            int rv = mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), NULL, 5000, &reply);
            if (rv == -1)
                zsys_error ("%s:\tgpio: mlm_client_sendto failed", self->name);

            zmsg_destroy (&reply);
        }
        else if (subject == "GPIO_TEMPLATE_ADD") {

            char *sensor_partnumber = zmsg_popstr (message);
            if (sensor_partnumber) {
                zconfig_t *root = zconfig_new ("root", NULL);
                string template_filename = string(self->template_dir) + string(sensor_partnumber) + string(".tpl");

                // We have a GPIO sensor, process it
                /*zconfig_t *sensor_template_file = zconfig_load (template_filename.c_str());
                if (!sensor_template_file) {
                    my_zsys_debug (self->verbose, "Can't create sensor template file"); // FIXME: error
                    zmsg_addstr (reply, "ERROR");
                    zmsg_addstr (reply, "?"); // FIXME: check errno for exact 
                }*/

                char *manufacturer = zmsg_popstr (message);
                char *type = zmsg_popstr (message);
                char *normal_state = zmsg_popstr (message);
                char *gpx_direction = zmsg_popstr (message);
                char *alarm_severity = zmsg_popstr (message);
                char *alarm_message = zmsg_popstr (message);
                
                // Sanity check
                if ( !type || !alarm_message) {
                    zmsg_addstr (reply, "ERROR");
                    zmsg_addstr (reply, "MISSING_PARAM");
                }
                else {
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
                    
                    zconfig_set_comment (root, " Generated through 42ITy web UI");
                    zconfig_put (root, "manufacturer", manufacturer);
                    zconfig_put (root, "part-number", sensor_partnumber);
                    zconfig_put (root, "type", type);
                    zconfig_put (root, "normal-state", normal_state);
                    zconfig_put (root, "gpx-direction", gpx_direction);
                    zconfig_put (root, "alarm-severity", alarm_severity);
                    zconfig_put (root, "alarm-message", alarm_message);

                    // Save the template
                    int rv = zconfig_save (root, template_filename.c_str());
                    zconfig_destroy (&root);

                    // Prepare our answer
                    if ( rv == 0)
                        zmsg_addstr (reply, "OK");
                    else {
                        zmsg_addstr (reply, "ERROR");
                        zmsg_addstr (reply, "UNKNOWN"); // FIXME: check errno
                    }
                    // Cleanup
                    zstr_free(&manufacturer);
                    zstr_free(&type);
                    zstr_free(&normal_state);
                    zstr_free(&gpx_direction);
                    zstr_free(&alarm_severity);
                    zstr_free(&alarm_message);
            }
            else {
                zmsg_addstr (reply, "ERROR");
                zmsg_addstr (reply, "MISSING_PARAM");
            }
            // send the reply
            int rv = mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), subject.c_str(), NULL, 5000, &reply);
            if (rv == -1)
                zsys_error ("%s:\tgpio: mlm_client_sendto failed", self->name);

            zstr_free(&sensor_partnumber);
            
        }

        else if (subject == "GPIO_TEST") {
            ;
        }
        zmsg_destroy (&reply);
    }
}

//  --------------------------------------------------------------------------
//  Create a new fty_sensor_gpio_server

fty_sensor_gpio_server_t *
fty_sensor_gpio_server_new (const char* name)
{
    fty_sensor_gpio_server_t *self = (fty_sensor_gpio_server_t *) zmalloc (sizeof (fty_sensor_gpio_server_t));
    assert (self);

    //  Initialize class properties
    self->mlm          = mlm_client_new();
    self->name         = strdup(name);
    self->verbose      = false;
    self->test_mode    = false;
    self->template_dir = NULL;
    self->gpio_lib = libgpio_new ();
    assert (self->gpio_lib);

    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the fty_sensor_gpio_server

void
fty_sensor_gpio_server_destroy (fty_sensor_gpio_server_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        fty_sensor_gpio_server_t *self = *self_p;

        //  Free class properties
        libgpio_destroy (&self->gpio_lib);
        zstr_free(&self->name);
        mlm_client_destroy (&self->mlm);
        if (self->template_dir)
            zstr_free(&self->template_dir);

        //  Free object itself
        free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Create a new fty_info_server

void
fty_sensor_gpio_server (zsock_t *pipe, void *args)
{
    char *name = (char *)args;
    if (!name) {
        zsys_error ("Adress for fty-sensor-gpio actor is NULL");
        return;
    }

    fty_sensor_gpio_server_t *self = fty_sensor_gpio_server_new(name);
    assert (self);

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (self->mlm), NULL);
    assert (poller);

    zsock_signal (pipe, 0); 
    zsys_info ("%s_server: Started", self->name);

    while (!zsys_interrupted)
    {
        void *which = zpoller_wait (poller, TIMEOUT_MS);
        if (which == NULL) {
            if (zpoller_terminated (poller) || zsys_interrupted) {
                break;
            }
        }
        if (which == pipe) {
            zmsg_t *message = zmsg_recv (pipe);
            char *cmd = zmsg_popstr (message);
            if (cmd) {
                my_zsys_debug(self->verbose, "fty_sensor_gpio: received command %s", cmd);
                if (streq (cmd, "$TERM")) {
                    zstr_free (&cmd);
                    zmsg_destroy (&message);
                    goto exit;
                }
                else if (streq (cmd, "CONNECT")) {
                    char *endpoint = zmsg_popstr (message);
                     if (!endpoint)
                        zsys_error ("%s:\tMissing endpoint", self->name);
                    assert (endpoint);
                    int r = mlm_client_connect (self->mlm, endpoint, 5000, self->name);
                    if (r == -1)
                        zsys_error ("%s:\tConnection to endpoint '%s' failed", self->name, endpoint);
                    zsys_debug("fty-gpio-sensor-server: CONNECT %s/%s", endpoint, self->name);
                    zstr_free (&endpoint);
                }
                else if (streq (cmd, "PRODUCER")) {
                    char *stream = zmsg_popstr (message);
                    assert (stream);
                    mlm_client_set_producer (self->mlm, stream);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: setting PRODUCER on %s", stream);
                    zstr_free (&stream);
                }
                else if (streq (cmd, "CONSUMER")) {
                    char *stream = zmsg_popstr (message);
                    char *pattern = zmsg_popstr (message);
                    assert (stream && pattern);
                    mlm_client_set_consumer (self->mlm, stream, pattern);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: setting CONSUMER on %s/%s", stream, pattern);
                    zstr_free (&stream);
                    zstr_free (&pattern);
                }
                else if (streq (cmd, "VERBOSE")) {
                    self->verbose = true;
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: VERBOSE=true");
                    libgpio_set_verbose(self->gpio_lib, self->verbose);
                }
                else if (streq (cmd, "TEST")) {
                    self->test_mode = true;
                    libgpio_set_test_mode (self->gpio_lib, self->test_mode);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: TEST=true");
                }
                else if (streq (cmd, "UPDATE")) {
                    s_check_gpio_status(self);
                }
                else if (streq (cmd, "TEMPLATE_DIR")) {
                    self->template_dir = zmsg_popstr (message);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: Using sensors template directory: %s", self->template_dir);
                }
                else if (streq (cmd, "GPIO_CHIP_ADDRESS")) {
                    char *str_gpio_base_address = zmsg_popstr (message);
                    int gpio_base_address = atoi(str_gpio_base_address);
                    libgpio_set_gpio_base_address (self->gpio_lib, gpio_base_address);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: GPIO_CHIP_ADDRESS=%i", gpio_base_address);
                    zstr_free (&str_gpio_base_address);
                }
                else if (streq (cmd, "GPI_OFFSET")) {
                    char *str_gpi_offset = zmsg_popstr (message);
                    int gpi_offset = atoi(str_gpi_offset);
                    libgpio_set_gpi_offset (self->gpio_lib, gpi_offset);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: GPI_OFFSET=%i", gpi_offset);
                    zstr_free (&str_gpi_offset);
                }
                else if (streq (cmd, "GPO_OFFSET")) {
                    char *str_gpo_offset = zmsg_popstr (message);
                    int gpo_offset = atoi(str_gpo_offset);
                    libgpio_set_gpo_offset (self->gpio_lib, gpo_offset);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: GPO_OFFSET=%i", gpo_offset);
                    zstr_free (&str_gpo_offset);
                }
                else if (streq (cmd, "GPI_COUNT")) {
                    char *str_gpi_count = zmsg_popstr (message);
                    int gpi_count = atoi(str_gpi_count);
                    libgpio_set_gpi_count (self->gpio_lib, gpi_count);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: GPI_COUNT=%i", gpi_count);
                    zstr_free (&str_gpi_count);
                }
                else if (streq (cmd, "GPO_COUNT")) {
                    char *str_gpo_count = zmsg_popstr (message);
                    int gpo_count = atoi(str_gpo_count);
                    libgpio_set_gpo_count (self->gpio_lib, gpo_count);
                    my_zsys_debug (self->verbose, "fty_sensor_gpio: GPO_COUNT=%i", gpo_count);
                    zstr_free (&str_gpo_count);
                }
                else {
                    zsys_warning ("%s:\tUnknown API command=%s, ignoring", __func__, cmd);
                }
                zstr_free (&cmd);
            }
            zmsg_destroy (&message);
        }
        else if (which == mlm_client_msgpipe (self->mlm)) {
            zmsg_t *message = mlm_client_recv (self->mlm);
            if (streq (mlm_client_command (self->mlm), "MAILBOX DELIVER")) {
                // someone is addressing us directly
                s_handle_mailbox(self, message);
            }
            zmsg_destroy (&message);
        }
    }
exit:
    zpoller_destroy (&poller);
    fty_sensor_gpio_server_destroy(&self);
}

//  --------------------------------------------------------------------------
//  Self test of this class

void
fty_sensor_gpio_server_test (bool verbose)
{
    printf (" * fty_sensor_gpio_server: ");

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase for the variables (assert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    // Uncomment these to use C++ strings in C++ selftest code:
    std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);
    assert ( (str_SELFTEST_DIR_RO != "") );
    assert ( (str_SELFTEST_DIR_RW != "") );
    // NOTE that for "char*" context you need (str_SELFTEST_DIR_RO + "/myfilename").c_str()

    //  @selftest

    static const char* endpoint = "inproc://fty_sensor_gpio_server_test";

    std::string template_dir = str_SELFTEST_DIR_RO + "/data/";
    zsys_dir_create (template_dir.c_str());
    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
        zstr_send (server, "VERBOSE");

    zactor_t *self = zactor_new (fty_sensor_gpio_server, (void*)FTY_SENSOR_GPIO_AGENT);
    assert (self);

    // Configure the server
    zstr_sendx (self, "CONNECT", endpoint, NULL);
    if (verbose)
        zstr_send (self, "VERBOSE");
    zstr_sendx (self, "PRODUCER", FTY_PROTO_STREAM_METRICS_SENSOR, NULL);
    zstr_sendx (self, "TEMPLATE_DIR", template_dir.c_str(), NULL);
    zstr_sendx (self, "GPIO_CHIP_ADDRESS", "488", NULL);
    zstr_sendx (self, "GPI_OFFSET", "-1", NULL);    // Only 1 GPI
    zstr_sendx (self, "GPO_OFFSET", "0", NULL);     // Only 1 GPO
    zstr_sendx (self, "GPI_COUNT", "10", NULL);
    zstr_sendx (self, "GPO_COUNT", "5", NULL);
    zstr_send (self, "TEST");

    mlm_client_t *mb_client = mlm_client_new ();
    mlm_client_connect (mb_client, endpoint, 1000, "fty_sensor_gpio_client");

    // Prepare the testbed with 2 assets (1xGPI + 1xGPO)
    fty_sensor_gpio_assets_t *assets_self = fty_sensor_gpio_assets_new("gpio-assets");

    int rv = add_sensor(assets_self, "create",
        "Eaton", "sensor-10", "GPIO-Sensor-Door1",
        "DCS001", "door-contact-sensor",
        "closed", "1",
        "GPI", "IPC1",
        "Door has been $status", "WARNING");
    assert (rv == 0);

    rv = add_sensor(assets_self, "create",
        "Eaton", "sensor-11", "GPIO-Test-GPO1",
        "DCS001", "dummy",
        "closed", "1",
        "GPO", "IPC1",
        "Dummy has been $status", "WARNING");
    assert (rv == 0);

    // Also create the dummy file for reading the GPI sensor
    std::string gpi_sys_dir = str_SELFTEST_DIR_RW + "/sys/class/gpio/gpio488";
    zsys_dir_create (gpi_sys_dir.c_str());
    std::string gpi1_fn = gpi_sys_dir + "/value";
    int handle = open (gpi1_fn.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
    assert (handle >= 0);
    int rc = write (handle, "0", 1);   // 0 == GPIO_STATE_CLOSED
    assert (rc == 1);
    close (handle);
    // and the path for GPO
    std::string gpo_sys_dir = str_SELFTEST_DIR_RW + "/sys/class/gpio/gpio489";
    zsys_dir_create (gpo_sys_dir.c_str());

    // Acquire the list of monitored sensors
    pthread_mutex_lock (&gpx_list_mutex);
    zlistx_t *test_gpx_list = get_gpx_list(verbose);
    assert (test_gpx_list);
    int sensors_count = zlistx_size (test_gpx_list);
    assert (sensors_count == 2);
    // Test the first sensor
/*    _gpx_info_t *gpx_info = (_gpx_info_t *)zlistx_first (test_gpx_list);
    assert (gpx_info);
    // Modify the current_state
    gpx_info->current_state = GPIO_STATE_OPENED;
*/
    pthread_mutex_unlock (&gpx_list_mutex);

// leak HERE begining
    // Test #1: Get status for an asset through its published metric
    {
        mlm_client_t *metrics_listener = mlm_client_new ();
        mlm_client_connect (metrics_listener, endpoint, 1000, "fty_sensor_gpio_metrics_listener");
        mlm_client_set_consumer (metrics_listener, FTY_PROTO_STREAM_METRICS_SENSOR, ".*");

        // Send an update and check for the generated metric
        zstr_sendx (self, "UPDATE", endpoint, NULL);
// leak HERE?!
//        zclock_sleep (500);

        // Check the published metric
        zmsg_t *recv = mlm_client_recv (metrics_listener);
        assert (recv);
        fty_proto_t *frecv = fty_proto_decode (&recv);
        assert (frecv);
        assert (streq (fty_proto_name (frecv), "sensor-10"));
        assert (streq (fty_proto_type (frecv), "status.GPI1"));
        assert (streq (fty_proto_aux_string (frecv, "port", NULL), "GPI1"));
        assert (streq (fty_proto_value (frecv), "closed"));

        fty_proto_destroy (&frecv);
        zmsg_destroy (&recv);
        mlm_client_destroy (&metrics_listener);
    }
// leak HERE end

    // Test #2: Post a GPIO_TEMPLATE_ADD request and check the file created
    // Note: this will serve afterward for the GPIO_MANIFEST / GPIO_MANIFEST_SUMMARY
    // requests
    {
        zmsg_t *msg = zmsg_new ();
        zmsg_addstr (msg, "TEST001");           // sensor_partnumber
        zmsg_addstr (msg, "FooManufacturer");   // manufacturer
        zmsg_addstr (msg, "test");              // type
        zmsg_addstr (msg, "closed");            // normal_state
        zmsg_addstr (msg, "GPI");               // gpx_direction
        zmsg_addstr (msg, "WARNING");           // alarm_severity
        zmsg_addstr (msg, "test triggered");    // alarm_message

        int rv = mlm_client_sendto (mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_TEMPLATE_ADD", NULL, 5000, &msg);
        assert ( rv == 0 );

        // Check the server answer
        zmsg_t *recv = mlm_client_recv (mb_client);
        assert(recv);
        char *answer = zmsg_popstr (recv);
        assert ( answer );
        zsys_debug("Got answer: '%s'", answer);
        assert ( streq (answer, "OK") );
        zstr_free(&answer);
        zmsg_destroy (&msg);
        zmsg_destroy (&recv);
    }

    // Test #3: Get GPIO_MANIFEST request and check it
    // Note: we should receive the template created above only!
    {
        zmsg_t *msg = zmsg_new ();
        int rv = mlm_client_sendto (mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_MANIFEST", NULL, 5000, &msg);
        assert ( rv == 0 );

        // Check the server answer
        zmsg_t *recv = mlm_client_recv (mb_client);
        assert(recv);
        char *recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "OK") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "TEST001") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "FooManufacturer") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "test") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "closed") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "GPI") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "WARNING") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "test triggered") );
        zstr_free (&recv_str);

        zmsg_destroy (&msg);
        zmsg_destroy (&recv);
    }

    // Test #4: Request GPIO_MANIFEST_SUMMARY and check it
    {
        zmsg_t *msg = zmsg_new ();
        int rv = mlm_client_sendto (mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_MANIFEST_SUMMARY", NULL, 5000, &msg);
        assert ( rv == 0 );

        // Check the server answer
        zmsg_t *recv = mlm_client_recv (mb_client);
        assert(recv);
        char *recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "OK") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "TEST001") );
        zstr_free (&recv_str);
        recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "FooManufacturer") );
        zstr_free (&recv_str);

        zmsg_destroy (&msg);
        zmsg_destroy (&recv);
    }
    // Test #5: Send GPO_INTERACTION request on GPO 'sensor-11' and check it
    {
        zmsg_t *msg = zmsg_new ();
        zmsg_addstr (msg, "sensor-11");     // sensor
        zmsg_addstr (msg, "open");          // action
        int rv = mlm_client_sendto (mb_client, FTY_SENSOR_GPIO_AGENT, "GPO_INTERACTION", NULL, 5000, &msg);
        assert ( rv == 0 );

        // Check the server answer
        zmsg_t *recv = mlm_client_recv (mb_client);
        assert(recv);
        char *recv_str = zmsg_popstr (recv);
        assert ( streq ( recv_str, "OK") );
        zstr_free (&recv_str);

        zmsg_destroy (&msg);
        zmsg_destroy (&recv);

        // Now check the filesystem
        std::string gpo1_fn = gpo_sys_dir + "/value";
        int handle = open (gpo1_fn.c_str(), O_RDONLY, 0);
        assert (handle >= 0);
        char readbuf[2];
        int rc = read (handle, &readbuf[0], 1);
        assert (rc == 1);
        close (handle);
        assert ( readbuf[0] == '1' ); // 1 == GPIO_STATE_OPENED
        
    }

    zsys_dir_delete (template_dir.c_str());
    // Delete all test files
    zdir_t *dir = zdir_new (template_dir.c_str(), NULL);
    assert (dir);
    zdir_remove (dir, true);
    zdir_destroy (&dir);
    dir = zdir_new ((str_SELFTEST_DIR_RW + "/sys").c_str(), NULL);
    assert (dir);
    zdir_remove (dir, true);
    zdir_destroy (&dir);

    // Cleanup assets
    fty_sensor_gpio_assets_destroy(&assets_self);

    // And connections / actors
    mlm_client_destroy (&mb_client);
    zactor_destroy (&self);
    zactor_destroy (&server);
    //  @end
    printf ("OK\n");
}
