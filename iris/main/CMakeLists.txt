idf_component_register(
    SRCS "iris_main.c"
    INCLUDE_DIRS "."
    REQUIRES mtkernel hal soc esp_rom esp_hw_support esp_timer esp_wifi esp_netif esp_event nvs_flash ieee802154
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_startup_start_app"
    "-Wl,--wrap=rtos_int_exit"
)
