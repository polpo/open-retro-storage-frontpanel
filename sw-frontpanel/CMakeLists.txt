# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.4.1-pre1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Name the firmware per product (picoide-frontpanel / bluescsi-frontpanel).
# One BlueSCSI binary covers v2 (I2C) and Ultra/Ultra Wide (SPI): the host
# transport is auto-detected at runtime and persisted in NVS.
if(SDKCONFIG_DEFAULTS MATCHES "bluescsi")
  set(FRONTPANEL_PRODUCT "bluescsi")
else()
  set(FRONTPANEL_PRODUCT "picoide")
endif()
project(${FRONTPANEL_PRODUCT}-frontpanel)
