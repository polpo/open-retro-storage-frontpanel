set(WWW_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/www")
set(WWW_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/www")
set(MINIFY_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/minify_www.py")
set(REQUIREMENTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../requirements.txt")
set(REQUIREMENTS_STAMP "${CMAKE_BINARY_DIR}/.requirements_installed")

# Install Python dependencies from requirements.txt (once per build dir)
if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT EXISTS "${REQUIREMENTS_STAMP}")
    message(STATUS "Installing Python dependencies from requirements.txt...")
    execute_process(
        COMMAND ${PYTHON} -m pip install --quiet --no-build-isolation -r "${REQUIREMENTS_FILE}"
        RESULT_VARIABLE PIP_RESULT
    )
    if(PIP_RESULT EQUAL 0)
        file(TOUCH "${REQUIREMENTS_STAMP}")
    else()
        message(WARNING "Failed to install Python dependencies - minification will be skipped")
    endif()
endif()

idf_component_register(SRCS "main.c"
                            "display_manager.c"
                            "gpio_handler.c"
                            "menu_system.c"
                            "host_comm.c"
                            "transport.c"
                            "transport_i2c.c"
                            "transport_spi.c"
                            "ui_screens.c"
                            "led_driver.c"
                            "wifi_manager.c"
                            "web_server.cpp"
                            "interface_common.c"
                            "json_stream.cpp"
                            "ota_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES u8g2 u8g2-hal-esp-idf esp_timer driver led_strip esp_wifi esp_netif esp_http_server nvs_flash json app_update mbedtls
                    EMBED_FILES "${WWW_BUILD_DIR}/index.html.gz"
                                "${WWW_BUILD_DIR}/app.js.gz"
                                "${WWW_BUILD_DIR}/sha256.js.gz"
                )

# Custom commands for minifying web assets (after idf_component_register)
set(WWW_FILES index.html app.js sha256.js)
foreach(file ${WWW_FILES})
    add_custom_command(
        OUTPUT "${WWW_BUILD_DIR}/${file}.gz"
        COMMAND ${PYTHON} "${MINIFY_SCRIPT}" "${WWW_SRC_DIR}/${file}" "${WWW_BUILD_DIR}/${file}.gz"
        DEPENDS "${WWW_SRC_DIR}/${file}" "${MINIFY_SCRIPT}"
        COMMENT "Minifying ${file}"
        VERBATIM
    )
endforeach()
