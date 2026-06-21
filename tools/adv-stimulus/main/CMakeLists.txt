idf_component_register(SRCS "adv_stimulus_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_driver_usb_serial_jtag)
