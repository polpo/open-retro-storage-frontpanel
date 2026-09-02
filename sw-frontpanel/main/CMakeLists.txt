set(WWW_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/www")
set(WWW_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/www")
set(MINIFY_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/minify_www.py")
set(REQUIREMENTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../requirements.txt")
set(REQUIREMENTS_STAMP "${CMAKE_BINARY_DIR}/.requirements_installed")

# Install Python dependencies from requirements.txt (once per build dir).
# If they're already importable (e.g. provided by the Nix dev shell via
# PYTHONPATH, where pip can't write to the immutable store), skip pip entirely.
if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT EXISTS "${REQUIREMENTS_STAMP}")
    execute_process(
        COMMAND ${PYTHON} -c "import preprocess, htmlmin, rjsmin"
        RESULT_VARIABLE DEPS_PRESENT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(DEPS_PRESENT EQUAL 0)
        message(STATUS "Web-asset Python dependencies already available - skipping pip install")
        file(TOUCH "${REQUIREMENTS_STAMP}")
    else()
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
endif()

idf_component_register(SRCS "main.c"
                            "display_manager.c"
                            "gpio_handler.c"
                            "menu_system.c"
                            "host_comm.c"
                            "panel_policy.c"
                            "host_link.c"
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
                            "fw_version.c"
                    INCLUDE_DIRS "."
                    REQUIRES u8g2 u8g2-hal-esp-idf esp_timer driver led_strip esp_wifi esp_netif esp_http_server nvs_flash json app_update mbedtls
                    EMBED_FILES "${WWW_BUILD_DIR}/index.html.gz"
                                "${WWW_BUILD_DIR}/app.js.gz"
                                "${WWW_BUILD_DIR}/sha256.js.gz"
                                "${WWW_BUILD_DIR}/logo.svg.gz"
                )

# Select product (web-asset preprocessing + logo) based on Kconfig
if(CONFIG_PRODUCT_BLUESCSI)
    set(WWW_PRODUCT "bluescsi")
    set(LOGO_SRC "bluescsi-logo.svg")
else()
    set(WWW_PRODUCT "picoide")
    set(LOGO_SRC "picoide-logo.svg")
endif()

# Depend on sdkconfig so assets are re-processed when the product changes,
# even if the source files themselves are untouched.
idf_build_get_property(SDKCONFIG_PATH SDKCONFIG)

# Custom commands for preprocessing + minifying web assets (after idf_component_register)
set(WWW_FILES index.html app.js sha256.js)
foreach(file ${WWW_FILES})
    add_custom_command(
        OUTPUT "${WWW_BUILD_DIR}/${file}.gz"
        COMMAND ${PYTHON} "${MINIFY_SCRIPT}" --product ${WWW_PRODUCT} "${WWW_SRC_DIR}/${file}" "${WWW_BUILD_DIR}/${file}.gz"
        DEPENDS "${WWW_SRC_DIR}/${file}" "${MINIFY_SCRIPT}" "${SDKCONFIG_PATH}"
        COMMENT "Processing ${file} (${WWW_PRODUCT})"
        VERBATIM
    )
endforeach()

# Gzip product logo (selected by Kconfig)
add_custom_command(
    OUTPUT "${WWW_BUILD_DIR}/logo.svg.gz"
    COMMAND ${PYTHON} "${MINIFY_SCRIPT}" --product ${WWW_PRODUCT} "${WWW_SRC_DIR}/${LOGO_SRC}" "${WWW_BUILD_DIR}/logo.svg.gz"
    DEPENDS "${WWW_SRC_DIR}/${LOGO_SRC}" "${MINIFY_SCRIPT}" "${SDKCONFIG_PATH}"
    COMMENT "Gzipping ${LOGO_SRC} as logo.svg"
    VERBATIM
)
