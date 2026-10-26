idf_component_register(
    SRCS "main.c"
         "wtf_safety.c"
         "wtf_discovery.c"
         "wtf_sniffer.c"
         "wtf_fuzzer.c"
         "wtf_storage.c"
         "wtf_telemetry.c"
         "wtf_dashboard.c"
    INCLUDE_DIRS "."
    REQUIRES mtkernel hal soc esp_driver_gpio esp_driver_uart esp_rom esp_hw_support esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_startup_start_app"
    "-Wl,--wrap=rtos_int_exit"
)
