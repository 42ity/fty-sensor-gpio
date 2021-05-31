cmake_minimum_required(VERSION 3.13)
##############################################################################################################

project(fty-sensor-gpio
  VERSION "1.0.0"
  DESCRIPTION "Agent to manage GPI sensors and GPO devices"
  LANGUAGES CXX)

##############################################################################################################
find_package(fty-cmake PATHS ${CMAKE_BINARY_DIR}/fty-cmake)
##############################################################################################################

etn_target(static ${PROJECT_NAME}-lib
    SOURCES
        src/fty_sensor_gpio_alerts.h
        src/fty_sensor_gpio_assets.cc
        src/fty_sensor_gpio_assets.h
        src/fty_sensor_gpio.h
        src/fty_sensor_gpio_server.cc
        src/fty_sensor_gpio_server.h
        src/libgpio.cc
        src/libgpio.h
    USES_PRIVATE
        czmq
        mlm
        fty_proto
        fty_common
        fty_common_logging
        fty_shm
    PRIVATE
)

##############################################################################################################

etn_target(exe ${PROJECT_NAME}
    SOURCES
        src/fty_sensor_gpio.cc
    USES_PRIVATE
        ${PROJECT_NAME}-lib
)

##############################################################################################################

etn_test_target(${PROJECT_NAME}-lib
    CONFIGS
        tests/selftest-ro/*
    SOURCES
        tests/main.cpp
        tests/sensor_gpio_assets.cpp
        tests/sensor_gpio_server.cpp
    PREPROCESSOR
        -DCATCH_CONFIG_FAST_COMPILE
    SUBDIR
        tests
)

##############################################################################################################

# Agent config
etn_configure_file(
    resources/${PROJECT_NAME}.cfg.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}"
)

# systemd unit
etn_configure_file(
    resources/${PROJECT_NAME}.service.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system/"
)

# systemd tmpfiles
etn_configure_file(
    resources/${PROJECT_NAME}.conf.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d/"
)

# udev rules files
etn_configure_file(
    resources/42ity-gpio-permissions.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "/lib/udev/"
)
etn_configure_file(
    resources/42-ity-gpio.rules.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/udev/rules.d/"
)

# GPI templates for known devices
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/DCS001.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/M12.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/VIB001.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/WLD012.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/XCELW.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/GPOGEN.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/MZXE-prealarm.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES ${PROJECT_SOURCE_DIR}/tests/selftest-ro/data/MZXE-alarm.tpl DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
