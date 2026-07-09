# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.4.0-pre3")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Name the firmware per host board so users can pick the right image
# (picoide-frontpanel / bluescsi-ultra-frontpanel / bluescsi-v2-frontpanel).
# BlueSCSI ships two transport flavors from the same product: SPI for
# Ultra/Ultra Wide and I2C (the ".i2c" defaults layer) for v2.
if(SDKCONFIG_DEFAULTS MATCHES "bluescsi")
  if(SDKCONFIG_DEFAULTS MATCHES "i2c")
    set(FRONTPANEL_PRODUCT "bluescsi-v2")
  else()
    set(FRONTPANEL_PRODUCT "bluescsi-ultra")
  endif()
else()
  set(FRONTPANEL_PRODUCT "picoide")
endif()
project(${FRONTPANEL_PRODUCT}-frontpanel)
