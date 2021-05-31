#include "src/libgpio.h"
#include <catch2/catch.hpp>

TEST_CASE("libgpio test")
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
