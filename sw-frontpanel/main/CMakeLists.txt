idf_component_register(SRCS "main.c" 
                            "display_manager.c"
                            "button_handler.c"
                            "menu_system.c"
                            "i2c_slave_comm.c"
                            "ui_screens.c"
                    INCLUDE_DIRS "."
                    REQUIRES u8g2 u8g2-hal-esp-idf esp_timer driver
                )
