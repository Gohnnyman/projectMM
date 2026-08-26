idf_component_register(
    SRCS "moonbase_main.cpp"
    INCLUDE_DIRS ""
    # esp_https_ota brings esp_http_client and esp-tls with it, so neither is listed separately.
    # esp_eth is absent on purpose: Ethernet support is a follow-up (the app's ethInit needs
    # per-board pin configuration), and requiring the component would link a driver nothing calls.
    REQUIRES nvs_flash esp_netif esp_event esp_wifi app_update esp_https_ota
             mbedtls esp_partition joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
