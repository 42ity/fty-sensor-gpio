#include "src/fty_sensor_gpio.h"
#include "src/fty_sensor_gpio_assets.h"
#include "src/fty_sensor_gpio_server.h"
#include "src/libgpio.h"
#include <catch2/catch.hpp>
#include <czmq.h>
#include <fty_proto.h>
#include <malamute.h>

extern zmsg_t* hw_cap_test_reply_gpi;
extern zmsg_t* hw_cap_test_reply_gpo;

void libgpio_test()
{
    const char* SELFTEST_DIR_RW = ".";

    // Note: for testing purpose, we use a trick, that is to access the /sys
    // FS under our SELFTEST_DIR_RW.

    //  @selftest
    //  Simple create/destroy test
    libgpio_t* self = libgpio_new();
    REQUIRE(self);
    libgpio_destroy(&self);

    // Setup
    self = libgpio_new();
    REQUIRE(self);
    libgpio_set_test_mode(self, true);
    libgpio_set_gpio_base_address(self, 0);
    // We use the same offset for GPI and GPO, to be able to write a GPO
    // and read the same for GPI
    libgpio_set_gpi_offset(self, 0);
    libgpio_set_gpo_offset(self, 0);
    libgpio_set_gpi_count(self, 10);
    libgpio_set_gpo_count(self, 5);

    // Write test
    // Let's first write to the dummy GPIO, so that the read works afterward
    CHECK(libgpio_write(self, 1, GPIO_STATE_CLOSED) == 0);

    // Read test
    CHECK(libgpio_read(self, 1, GPIO_DIRECTION_IN) == GPIO_STATE_CLOSED);

    // Value resolution test
    CHECK(libgpio_get_status_value("opened") == GPIO_STATE_OPENED);
    CHECK(libgpio_get_status_value("closed") == GPIO_STATE_CLOSED);
    CHECK(libgpio_get_status_value(libgpio_get_status_string(GPIO_STATE_CLOSED).c_str()) == GPIO_STATE_CLOSED);

    // Delete all test files
    std::string sys_fn = std::string(SELFTEST_DIR_RW) + "/sys";
    zdir_t*     dir    = zdir_new(sys_fn.c_str(), NULL);
    CHECK(dir);
    zdir_remove(dir, true);
    zdir_destroy(&dir);

    libgpio_destroy(&self);
}


TEST_CASE("sensor gpio server test")
{
    libgpio_test();
    // FIXME: disable -server test for now, while waiting to catch
    // the malamute race-cond leak
    // See https://github.com/42ity/fty-sensor-gpio/issues/11
    // printf ("OK\n");
    // return;

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // selftest-ro; if your test creates filesystem objects, please
    // do so under selftest-rw. They are defined below along with a
    // usecase for the variables (CHECK) to make compilers happy.
    const char* SELFTEST_DIR_RW = ".";

    // Uncomment these to use C++ std::strings in C++ selftest code:
    std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);

    //  @selftest
    static const char* endpoint = "inproc://fty_sensor_gpio_server_test";

    // Note: here we test the creation of a template (GPIO_TEMPLATE_ADD)
    // and then the fact that GPIO_MANIFEST request just return this newly
    // created template!
    std::string template_dir = str_SELFTEST_DIR_RW + "/data/";
    zsys_dir_create(template_dir.c_str());
    zactor_t* server = zactor_new(mlm_server, const_cast<char*>("Malamute"));
    zstr_sendx(server, "BIND", endpoint, nullptr);

    zactor_t* self = zactor_new(fty_sensor_gpio_server, const_cast<char*>(FTY_SENSOR_GPIO_AGENT));
    REQUIRE(self);

    // Forge a HW_CAP reply message
    // msg-correlation-id'/OK/'type'/'count'/'base_address'/'offset'/'mapping1'/'mapping_val1'/'mapping2'/'mapping_val2'/
    // ...
    hw_cap_test_reply_gpi = zmsg_new();
    hw_cap_test_reply_gpo = zmsg_new();
    // zuuid_t can be omitted, since it's already been pop'ed
    // Same for "OK"
    // GPI
    // Offset '-1' means GPI '1' address is '488'
    zmsg_addstr(hw_cap_test_reply_gpi, "gpi");
    zmsg_addstr(hw_cap_test_reply_gpi, "10");
    zmsg_addstr(hw_cap_test_reply_gpi, "488");
    zmsg_addstr(hw_cap_test_reply_gpi, "-1");
    // GPO
    // We'll use GPO port '2', so address will be '490'
    zmsg_addstr(hw_cap_test_reply_gpo, "gpo");
    zmsg_addstr(hw_cap_test_reply_gpo, "5");
    zmsg_addstr(hw_cap_test_reply_gpo, "488");
    zmsg_addstr(hw_cap_test_reply_gpo, "0");
    // FIXME: add some mapping for testing
    zmsg_addstr(hw_cap_test_reply_gpo, "p4");
    zmsg_addstr(hw_cap_test_reply_gpo, "502");
    zmsg_addstr(hw_cap_test_reply_gpo, "p5");
    zmsg_addstr(hw_cap_test_reply_gpo, "503");

    // Configure the server
    // TEST *MUST* be set first, before HW_CAP, for HW capabilities
    zstr_sendx(self, "TEST", nullptr);
    zstr_sendx(self, "CONNECT", endpoint, nullptr);
    zstr_sendx(self, "PRODUCER", FTY_PROTO_STREAM_METRICS_SENSOR, nullptr);
    zstr_sendx(self, "TEMPLATE_DIR", template_dir.c_str(), nullptr);
    zstr_sendx(self, "HW_CAP", nullptr);
    mlm_client_t* mb_client = mlm_client_new();
    mlm_client_connect(mb_client, endpoint, 1000, "fty_sensor_gpio_client");

    // Prepare the testbed with 2 assets (1xGPI + 1xGPO)
    fty_sensor_gpio_assets_t* assets_self = fty_sensor_gpio_assets_new("gpio-assets");

    int rv = add_sensor(assets_self, "create", "Eaton", "sensorgpio-10", "GPIO-Sensor-Door1", "DCS001",
        "door-contact-sensor", "closed", "1", "GPI", "IPC1", "Rack1", "", "Door has been $status", "WARNING");
    REQUIRE(rv == 0);

    rv = add_sensor(assets_self, "create", "Eaton", "gpo-11", "GPIO-Test-GPO1", "DCS001", "dummy", "closed", "2", "GPO",
        "IPC1", "Room1", "", "Dummy has been $status", "WARNING");
    REQUIRE(rv == 0);

    // Also create the dummy file for reading the GPI sensor
    std::string gpi_sys_dir = str_SELFTEST_DIR_RW + "/sys/class/gpio/gpio488";
    zsys_dir_create(gpi_sys_dir.c_str());
    std::string gpi1_fn = gpi_sys_dir + "/value";
    int         handle  = open(gpi1_fn.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
    REQUIRE(handle >= 0);
    int rc = int(write(handle, "0", 1)); // 0 == GPIO_STATE_CLOSED
    REQUIRE(rc == 1);
    close(handle);
    // and the path for GPO
    std::string gpo_sys_dir = str_SELFTEST_DIR_RW + "/sys/class/gpio/gpio490";
    zsys_dir_create(gpo_sys_dir.c_str());
    std::string gpo_mapping_sys_dir = str_SELFTEST_DIR_RW + "/sys/class/gpio/gpio503";
    zsys_dir_create(gpo_mapping_sys_dir.c_str());

    // Acquire the list of monitored sensors
    pthread_mutex_lock(&gpx_list_mutex);
    zlistx_t* test_gpx_list = get_gpx_list();
    REQUIRE(test_gpx_list);
    int sensors_count = int(zlistx_size(test_gpx_list));
    CHECK(sensors_count == 2);
    // Test the first sensor
    /*    _gpx_info_t *gpx_info = (_gpx_info_t *)zlistx_first (test_gpx_list);
        CHECK (gpx_info);
        // Modify the current_state
        gpx_info->current_state = GPIO_STATE_OPENED;
    */
    pthread_mutex_unlock(&gpx_list_mutex);

    // Test #1: Get status for an asset through its published metric
    {
        zmsg_t* msg = zmsg_new();
        zmsg_addstr(msg, "sensorgpio-11");
        zmsg_addstr(msg, "2");
        zmsg_addstr(msg, "closed");
        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPOSTATE", nullptr, 5000, &msg);

        REQUIRE(rv == 0); // no response

        mlm_client_t* metrics_listener = mlm_client_new();
        mlm_client_connect(metrics_listener, endpoint, 1000, "fty_sensor_gpio_metrics_listener");
        mlm_client_set_consumer(metrics_listener, FTY_PROTO_STREAM_METRICS_SENSOR, ".*");
        zclock_sleep(1000);
        // Send an update and check for the generated metric
        zstr_sendx(self, "UPDATE", endpoint, nullptr);
        // Check the published metric
        zmsg_t* recv = mlm_client_recv(metrics_listener);
        REQUIRE(recv);
        fty_proto_t* frecv = fty_proto_decode(&recv);
        REQUIRE(frecv);
        CHECK(streq(fty_proto_name(frecv), "IPC1"));
        CHECK(streq(fty_proto_type(frecv), "status.GPI1"));
        CHECK(streq(fty_proto_aux_string(frecv, "port", nullptr), "GPI1"));
        CHECK(streq(fty_proto_value(frecv), "closed"));
        CHECK(streq(fty_proto_aux_string(frecv, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, nullptr), "sensorgpio-10"));

        fty_proto_destroy(&frecv);
        zmsg_destroy(&recv);

        recv = mlm_client_recv(metrics_listener);
        REQUIRE(recv);
        frecv = fty_proto_decode(&recv);
        REQUIRE(frecv);
        CHECK(streq(fty_proto_name(frecv), "IPC1"));
        CHECK(streq(fty_proto_type(frecv), "status.GPO2"));
        CHECK(streq(fty_proto_aux_string(frecv, "port", nullptr), "GPO2"));
        CHECK(streq(fty_proto_value(frecv), "closed"));
        CHECK(streq(fty_proto_aux_string(frecv, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, nullptr), "gpo-11"));
        fty_proto_destroy(&frecv);
        zmsg_destroy(&recv);
        zmsg_destroy(&msg);

        mlm_client_destroy(&metrics_listener);
    }

    // Test #2: Post a GPIO_TEMPLATE_ADD request and check the file created
    // Note: this will serve afterward for the GPIO_MANIFEST / GPIO_MANIFEST_SUMMARY
    // requests
    {
        zmsg_t*  msg   = zmsg_new();
        zuuid_t* zuuid = zuuid_new();
        zmsg_addstr(msg, zuuid_str_canonical(zuuid));
        zmsg_addstr(msg, "TEST001");         // sensor_partnumber
        zmsg_addstr(msg, "FooManufacturer"); // manufacturer
        zmsg_addstr(msg, "test");            // type
        zmsg_addstr(msg, "closed");          // normal_state
        zmsg_addstr(msg, "GPI");             // gpx_direction
        zmsg_addstr(msg, "internal");        // power_source
        zmsg_addstr(msg, "WARNING");         // alarm_severity
        zmsg_addstr(msg, "test triggered");  // alarm_message

        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_TEMPLATE_ADD", nullptr, 5000, &msg);
        REQUIRE(rv == 0);

        // Check the server answer
        zmsg_t* recv = mlm_client_recv(mb_client);
        REQUIRE(recv);
        char* answer = zmsg_popstr(recv);
        CHECK(streq(zuuid_str_canonical(zuuid), answer));
        zstr_free(&answer);
        answer = zmsg_popstr(recv);
        CHECK(answer);
        CHECK(streq(answer, "OK"));
        zstr_free(&answer);

        zuuid_destroy(&zuuid);
        zmsg_destroy(&recv);
        zmsg_destroy(&msg);
    }

    // Test #3: Get GPIO_MANIFEST request and check it
    // Note: we should receive the template created above only!
    {
        zmsg_t*  msg   = zmsg_new();
        zuuid_t* zuuid = zuuid_new();
        zmsg_addstr(msg, zuuid_str_canonical(zuuid));
        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_MANIFEST", nullptr, 5000, &msg);
        REQUIRE(rv == 0);

        // Check the server answer
        zmsg_t* recv = mlm_client_recv(mb_client);
        REQUIRE(recv);
        char* recv_str = zmsg_popstr(recv);
        CHECK(streq(zuuid_str_canonical(zuuid), recv_str));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "OK"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "TEST001"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "FooManufacturer"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "test"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "closed"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "GPI"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "internal"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "WARNING"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "test triggered"));
        zstr_free(&recv_str);

        zuuid_destroy(&zuuid);
        zmsg_destroy(&recv);
    }

    // Test #4: Request GPIO_MANIFEST_SUMMARY and check it
    {
        zmsg_t*  msg   = zmsg_new();
        zuuid_t* zuuid = zuuid_new();
        zmsg_addstr(msg, zuuid_str_canonical(zuuid));

        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPIO_MANIFEST_SUMMARY", nullptr, 5000, &msg);
        REQUIRE(rv == 0);

        // Check the server answer
        zmsg_t* recv = mlm_client_recv(mb_client);
        REQUIRE(recv);
        char* recv_str = zmsg_popstr(recv);
        CHECK(streq(zuuid_str_canonical(zuuid), recv_str));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "OK"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "TEST001"));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "FooManufacturer"));
        zstr_free(&recv_str);

        zuuid_destroy(&zuuid);
        zmsg_destroy(&recv);
    }

    // Test #5: Send GPO_INTERACTION request on GPO 'gpo-11' and check it
    {
        zmsg_t*  msg   = zmsg_new();
        zuuid_t* zuuid = zuuid_new();
        zmsg_addstr(msg, zuuid_str_canonical(zuuid));
        zmsg_addstr(msg, "gpo-11"); // sensor
        zmsg_addstr(msg, "open");   // action
        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPO_INTERACTION", nullptr, 5000, &msg);
        REQUIRE(rv == 0);

        // Check the server answer
        zmsg_t* recv = mlm_client_recv(mb_client);
        REQUIRE(recv);
        char* recv_str = zmsg_popstr(recv);
        CHECK(streq(zuuid_str_canonical(zuuid), recv_str));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "OK"));
        zstr_free(&recv_str);
        zuuid_destroy(&zuuid);
        zmsg_destroy(&recv);

        // Now check the filesystem
        std::string gpo1_fn = gpo_sys_dir + "/value";
        handle              = open(gpo1_fn.c_str(), O_RDONLY, 0);
        REQUIRE(handle >= 0);
        char readbuf[2];
        rc = int(read(handle, &readbuf[0], 1));
        CHECK(rc == 1);
        close(handle);
        CHECK(readbuf[0] == '1'); // 1 == GPIO_STATE_OPENED
    }

    // Test #6: Add another GPO (5) to test the special pin mapping
    // end GPO_INTERACTION request on GPO 'gpo-12' and check it
    {
        rv = add_sensor(assets_self, "create", "Eaton", "gpo-12", "GPIO-Test-GPO2", "DCS001", "dummy", "closed", "5",
            "GPO", "IPC1", "Room1", "", "Dummy has been $status", "WARNING");
        REQUIRE(rv == 0);

        zmsg_t*  msg   = zmsg_new();
        zuuid_t* zuuid = zuuid_new();
        zmsg_addstr(msg, zuuid_str_canonical(zuuid));
        zmsg_addstr(msg, "gpo-12"); // sensor
        zmsg_addstr(msg, "open");   // action
        rv = mlm_client_sendto(mb_client, FTY_SENSOR_GPIO_AGENT, "GPO_INTERACTION", nullptr, 5000, &msg);
        REQUIRE(rv == 0);

        // Check the server answer
        zmsg_t* recv = mlm_client_recv(mb_client);
        REQUIRE(recv);
        char* recv_str = zmsg_popstr(recv);
        CHECK(streq(zuuid_str_canonical(zuuid), recv_str));
        zstr_free(&recv_str);
        recv_str = zmsg_popstr(recv);
        CHECK(streq(recv_str, "OK"));
        zstr_free(&recv_str);
        zuuid_destroy(&zuuid);
        zmsg_destroy(&recv);

        // Now check the filesystem
        std::string gpo2_fn = gpo_mapping_sys_dir + "/value";
        handle              = open(gpo2_fn.c_str(), O_RDONLY, 0);
        REQUIRE(handle >= 0);
        char readbuf[2];
        rc = int(read(handle, &readbuf[0], 1));
        REQUIRE(rc == 1);
        close(handle);
        CHECK(readbuf[0] == '1'); // 1 == GPIO_STATE_OPENED
    }

    // Test #7: Disable all GPI/GPO (as on OVA),
    // Create a sensor and verify that it fails
    {
        // Forge the HW_CAP messages
        hw_cap_test_reply_gpi = zmsg_new();
        hw_cap_test_reply_gpo = zmsg_new();
        zmsg_addstr(hw_cap_test_reply_gpi, "gpi");
        zmsg_addstr(hw_cap_test_reply_gpi, "0");
        zmsg_addstr(hw_cap_test_reply_gpo, "gpo");
        zmsg_addstr(hw_cap_test_reply_gpo, "0");
        // Update our -server
        zstr_sendx(self, "HW_CAP", nullptr);

        zclock_sleep(500);

        rv = add_sensor(assets_self, "create", "Eaton", "gpo-13", "GPIO-Test-GPO2", "DCS001", "dummy", "closed", "1",
            "GPO", "IPC1", "Room1", "", "Dummy has been $status", "WARNING");
        REQUIRE(rv == 1);
    }

    zsys_dir_delete(template_dir.c_str());
    // Delete all test files
    zdir_t* dir = zdir_new(template_dir.c_str(), nullptr);
    REQUIRE(dir);
    zdir_remove(dir, true);
    zdir_destroy(&dir);
    dir = zdir_new((str_SELFTEST_DIR_RW + "/sys").c_str(), nullptr);
    REQUIRE(dir);
    zdir_remove(dir, true);
    zdir_destroy(&dir);
    zmsg_destroy(&hw_cap_test_reply_gpi);
    zmsg_destroy(&hw_cap_test_reply_gpo);

    // Cleanup assets
    fty_sensor_gpio_assets_destroy(&assets_self);

    // And connections / actors
    mlm_client_destroy(&mb_client);
    zactor_destroy(&self);
    zactor_destroy(&server);
    //  @end
    printf("OK\n");
}
