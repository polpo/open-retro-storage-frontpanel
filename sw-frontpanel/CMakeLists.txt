# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.4.1-pre2")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Name the firmware per product (picoide-frontpanel / bluescsi-frontpanel).
# One BlueSCSI binary covers v2 (I2C) and Ultra/Ultra Wide (SPI): the host
# transport is auto-detected at runtime and persisted in NVS.
if(SDKCONFIG_DEFAULTS MATCHES "bluescsi")
  set(FRONTPANEL_PRODUCT "bluescsi")
else()
  set(FRONTPANEL_PRODUCT "picoide")
endif()

# A bench build carries the test hooks and must never be mistaken for, or
# released as, the shipping image, so it gets its own name.
if(SDKCONFIG_DEFAULTS MATCHES "bench")
  set(FRONTPANEL_SUFFIX "-bench")
else()
  set(FRONTPANEL_SUFFIX "")
endif()
project(${FRONTPANEL_PRODUCT}-frontpanel${FRONTPANEL_SUFFIX})
