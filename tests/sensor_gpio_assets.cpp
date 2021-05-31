#include "src/fty_sensor_gpio.h"
#include "src/fty_sensor_gpio_assets.h"
#include "src/libgpio.h"
#include <catch2/catch.hpp>
#include <fty_log.h>
#include <fty_proto.h>
#include <malamute.h>

TEST_CASE("sensor gpio assets test", "[.]")
{
    // Note: If your selftest reads SCMed fixture data, please keep it in
    // selftest-ro; if your test creates filesystem objects, please
    // do so under selftest-rw. They are defined below along with a
    // usecase for the variables (CHECK) to make compilers happy.
    // const char *SELFTEST_DIR_RO = "selftest-ro";
    // Note: here, we use the templates from data to check if assets
    // are GPIOs
    const char* SELFTEST_DIR_RO = "tests/selftest-ro";

    char* test_data_dir = zsys_sprintf("%s/data/", SELFTEST_DIR_RO);
    REQUIRE(test_data_dir != nullptr);

    static const char* endpoint = "inproc://fty_sensor_gpio_assets_test";

    zactor_t* server = zactor_new(mlm_server, const_cast<char*>("Malamute"));
    zstr_sendx(server, "BIND", endpoint, nullptr);

    zactor_t* assets = zactor_new(fty_sensor_gpio_assets, const_cast<char*>("gpio-assets"));
    zstr_sendx(assets, "TEMPLATE_DIR", test_data_dir, nullptr);

    zstr_sendx(assets, "TEST", nullptr);
    zstr_sendx(assets, "CONNECT", endpoint, nullptr);
    zstr_sendx(assets, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", nullptr);
    // Use source-provided templates
    zclock_sleep(1000);

    mlm_client_t* asset_generator = mlm_client_new();
    mlm_client_connect(asset_generator, endpoint, 1000, "fty_sensor_gpio_assets_generator");
    mlm_client_set_producer(asset_generator, FTY_PROTO_STREAM_ASSETS);

    // Test #1: inject a basic list of assets and check it
    {
        // Asset 1: DCS001
        zhash_t* aux = zhash_new();
        zhash_t* ext = zhash_new();
        zhash_autofree(aux);
        zhash_autofree(ext);
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("sensorgpio"));
        zhash_update(aux, "status", const_cast<char*>("active"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPIO-Sensor-Door1"));
        zhash_update(ext, "port", const_cast<char*>("1"));
        zhash_update(ext, "model", const_cast<char*>("DCS001"));
        zhash_update(ext, "logical_asset", const_cast<char*>("Rack1"));

        zmsg_t* msg = fty_proto_encode_asset(aux, "sensorgpio-10", FTY_PROTO_ASSET_OP_CREATE, ext);

        int rv = mlm_client_send(asset_generator, "device.sensorgpio@sensorgpio-10", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Asset 2: WLD012
        aux = zhash_new();
        ext = zhash_new();
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("sensorgpio"));
        zhash_update(aux, "status", const_cast<char*>("active"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPIO-Sensor-Waterleak1"));
        zhash_update(ext, "port", const_cast<char*>("2"));
        zhash_update(ext, "model", const_cast<char*>("WLD012"));
        zhash_update(ext, "logical_asset", const_cast<char*>("Room1"));

        msg = fty_proto_encode_asset(aux, "sensorgpio-11", FTY_PROTO_ASSET_OP_CREATE, ext);

        rv = mlm_client_send(asset_generator, "device.sensorgpio@sensorgpio-11", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Asset 3: GPO-Beacon
        aux = zhash_new();
        ext = zhash_new();
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("gpo"));
        zhash_update(aux, "status", const_cast<char*>("active"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPO-Beacon"));
        zhash_update(ext, "port", const_cast<char*>("2"));

        msg = fty_proto_encode_asset(aux, "gpo-12", FTY_PROTO_ASSET_OP_CREATE, ext);

        rv = mlm_client_send(asset_generator, "device.gpo@gpo-12", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Asset 4: inactive GPO-Beacon
        aux = zhash_new();
        ext = zhash_new();
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("gpo"));
        zhash_update(aux, "status", const_cast<char*>("nonactive"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPO-Beacon"));
        zhash_update(ext, "port", const_cast<char*>("3"));

        msg = fty_proto_encode_asset(aux, "gpo-13", FTY_PROTO_ASSET_OP_CREATE, ext);

        rv = mlm_client_send(asset_generator, "device.gpo@gpo-13", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Check the result list
        pthread_mutex_lock(&gpx_list_mutex);
        zlistx_t* test_gpx_list = get_gpx_list();
        REQUIRE(test_gpx_list);
        int sensors_count = int(zlistx_size(test_gpx_list));
        REQUIRE(sensors_count == 3);
        // Test the first sensor
        gpx_info_t* gpx_info = static_cast<gpx_info_t*>(zlistx_first(test_gpx_list));
        REQUIRE(gpx_info);
        CHECK(streq(gpx_info->asset_name, "sensorgpio-10"));
        CHECK(streq(gpx_info->ext_name, "GPIO-Sensor-Door1"));
        CHECK(streq(gpx_info->part_number, "DCS001"));
        CHECK(gpx_info->gpx_number == 1);
        CHECK(streq(gpx_info->parent, "rackcontroller-1"));
        CHECK(streq(gpx_info->location, "Rack1"));
        // Acquired through the template file
        CHECK(streq(gpx_info->manufacturer, "Eaton"));
        CHECK(streq(gpx_info->type, "door-contact-sensor"));
        CHECK(gpx_info->normal_state == GPIO_STATE_CLOSED);
        CHECK(gpx_info->gpx_direction == GPIO_DIRECTION_IN);
        CHECK(streq(gpx_info->alarm_severity, "WARNING"));
        CHECK(streq(gpx_info->alarm_message, "Door has been $status"));

        // Test the 2nd sensor
        gpx_info = static_cast<gpx_info_t*>(zlistx_next(test_gpx_list));
        REQUIRE(gpx_info);
        CHECK(streq(gpx_info->asset_name, "sensorgpio-11"));
        CHECK(streq(gpx_info->ext_name, "GPIO-Sensor-Waterleak1"));
        CHECK(streq(gpx_info->part_number, "WLD012"));
        CHECK(gpx_info->gpx_number == 2);
        CHECK(streq(gpx_info->parent, "rackcontroller-1"));
        CHECK(streq(gpx_info->location, "Room1"));
        // Acquired through the template file
        CHECK(streq(gpx_info->manufacturer, "Eaton"));
        CHECK(streq(gpx_info->type, "water-leak-detector"));
        CHECK(gpx_info->normal_state == GPIO_STATE_OPENED);
        CHECK(gpx_info->gpx_direction == GPIO_DIRECTION_IN);

        // Test the GPO
        gpx_info = static_cast<gpx_info_t*>(zlistx_next(test_gpx_list));
        REQUIRE(gpx_info);
        CHECK(streq(gpx_info->asset_name, "gpo-12"));
        CHECK(streq(gpx_info->ext_name, "GPO-Beacon"));
        CHECK(gpx_info->gpx_number == 2);
        CHECK(streq(gpx_info->parent, "rackcontroller-1"));
        CHECK(gpx_info->normal_state == GPIO_STATE_CLOSED);
        CHECK(gpx_info->gpx_direction == GPIO_DIRECTION_OUT);

        pthread_mutex_unlock(&gpx_list_mutex);
    }

    // Test #2: Using the list of assets from #1, delete asset 3 and check the list
    {
        // Asset 1: DCS001
        zhash_t* aux = zhash_new();
        zhash_t* ext = zhash_new();
        zhash_autofree(aux);
        zhash_autofree(ext);
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("gpo"));

        zmsg_t* msg = fty_proto_encode_asset(aux, "gpo-12", FTY_PROTO_ASSET_OP_DELETE, ext);

        int rv = mlm_client_send(asset_generator, "device.gpo@gpo-12", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Check the result list
        pthread_mutex_lock(&gpx_list_mutex);
        zlistx_t* test_gpx_list = get_gpx_list();
        REQUIRE(test_gpx_list);
        int sensors_count = int(zlistx_size(test_gpx_list));
        CHECK(sensors_count == 2);

        pthread_mutex_unlock(&gpx_list_mutex);
    }
    // Test #3: Using the list of assets from #1, update asset 1 with overriden
    // 'normal-state' and check the list
    {
        // Asset 1: DCS001
        zhash_t* aux = zhash_new();
        zhash_t* ext = zhash_new();
        zhash_autofree(aux);
        zhash_autofree(ext);
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("sensorgpio"));
        zhash_update(aux, "status", const_cast<char*>("active"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPIO-Sensor-Door1"));
        zhash_update(ext, "normal_state", const_cast<char*>("opened"));
        zhash_update(ext, "port", const_cast<char*>("1"));
        zhash_update(ext, "model", const_cast<char*>("DCS001"));
        zhash_update(ext, "logical_asset", const_cast<char*>("Rack2"));

        zmsg_t* msg = fty_proto_encode_asset(aux, "sensorgpio-10", FTY_PROTO_ASSET_OP_UPDATE, ext);

        int rv = mlm_client_send(asset_generator, "device.sensorgpio@sensorgpio-10", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Check the result list
        pthread_mutex_lock(&gpx_list_mutex);
        zlistx_t* test_gpx_list = get_gpx_list();
        REQUIRE(test_gpx_list);
        int sensors_count = int(zlistx_size(test_gpx_list));
        REQUIRE(sensors_count == 2);
        // Only test the first sensor
        gpx_info_t* gpx_info = static_cast<gpx_info_t*>(zlistx_first(test_gpx_list));
        REQUIRE(gpx_info);
        gpx_info = static_cast<gpx_info_t*>(zlistx_next(test_gpx_list));
        REQUIRE(gpx_info);
        CHECK(streq(gpx_info->asset_name, "sensorgpio-10"));
        CHECK(streq(gpx_info->ext_name, "GPIO-Sensor-Door1"));
        CHECK(streq(gpx_info->part_number, "DCS001"));
        CHECK(gpx_info->gpx_number == 1);
        CHECK(streq(gpx_info->parent, "rackcontroller-1"));
        CHECK(streq(gpx_info->location, "Rack2"));
        // Main point: normal_state is now "opened"!
        CHECK(gpx_info->normal_state == GPIO_STATE_OPENED);
        // Other data are unchanged
        CHECK(streq(gpx_info->manufacturer, "Eaton"));
        CHECK(streq(gpx_info->type, "door-contact-sensor"));
        CHECK(gpx_info->gpx_direction == GPIO_DIRECTION_IN);
        CHECK(streq(gpx_info->alarm_severity, "WARNING"));
        CHECK(streq(gpx_info->alarm_message, "Door has been $status"));

        pthread_mutex_unlock(&gpx_list_mutex);
    }

    // Test #4: Using the list of assets from #1, delete asset 1 and check the list
    {
        // Asset 1: DCS001
        zhash_t* aux = zhash_new();
        zhash_t* ext = zhash_new();
        zhash_autofree(aux);
        zhash_autofree(ext);
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("sensorgpio"));

        zmsg_t* msg = fty_proto_encode_asset(aux, "sensorgpio-10", FTY_PROTO_ASSET_OP_DELETE, ext);

        int rv = mlm_client_send(asset_generator, "device.sensorgpio@sensorgpio-10", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Check the result list
        pthread_mutex_lock(&gpx_list_mutex);
        zlistx_t* test_gpx_list = get_gpx_list();
        REQUIRE(test_gpx_list);
        int sensors_count = int(zlistx_size(test_gpx_list));
        REQUIRE(sensors_count == 1);
        // There must remain only 'sensorgpio-11'
        gpx_info_t* gpx_info = static_cast<gpx_info_t*>(zlistx_first(test_gpx_list));
        REQUIRE(gpx_info);
        CHECK(streq(gpx_info->asset_name, "sensorgpio-11"));

        pthread_mutex_unlock(&gpx_list_mutex);
    }

    // Test #5: Using the list of assets from #1, update asset 2 with
    // 'status=nonactive' and check the list
    {
        // Asset 1: DCS001
        zhash_t* aux = zhash_new();
        zhash_t* ext = zhash_new();
        zhash_autofree(aux);
        zhash_autofree(ext);
        zhash_update(aux, "type", const_cast<char*>("device"));
        zhash_update(aux, "subtype", const_cast<char*>("sensorgpio"));
        zhash_update(aux, "status", const_cast<char*>("nonactive"));
        zhash_update(aux, "parent_name.1", const_cast<char*>("rackcontroller-1"));
        zhash_update(ext, "name", const_cast<char*>("GPIO-Sensor-Waterleak1"));
        zhash_update(ext, "port", const_cast<char*>("2"));
        zhash_update(ext, "model", const_cast<char*>("WLD012"));
        zhash_update(ext, "logical_asset", const_cast<char*>("Room1"));

        zmsg_t* msg = fty_proto_encode_asset(aux, "sensorgpio-11", FTY_PROTO_ASSET_OP_UPDATE, ext);

        int rv = mlm_client_send(asset_generator, "device.sensorgpio@sensorgpio-11", &msg);
        REQUIRE(rv == 0);
        zhash_destroy(&aux);
        zhash_destroy(&ext);
        zclock_sleep(1000);
        zmsg_destroy(&msg);

        // Check the result list
        pthread_mutex_lock(&gpx_list_mutex);
        zlistx_t* test_gpx_list = get_gpx_list();
        REQUIRE(test_gpx_list);
        int sensors_count = int(zlistx_size(test_gpx_list));
        REQUIRE(sensors_count == 0);

        pthread_mutex_unlock(&gpx_list_mutex);
    }

    zstr_free(&test_data_dir);
    mlm_client_destroy(&asset_generator);
    zactor_destroy(&assets);
    zactor_destroy(&server);
}
