idf_component_register(
    SRCS "moonbase_main.cpp"
    INCLUDE_DIRS ""
    # The app's own logo, embedded IDF-natively; served as /logo.png (header + favicon).
    EMBED_FILES "../../src/ui/moonlight-logo.png"
    # esp_https_ota brings esp_http_client and esp-tls with it, so neither is listed separately.
    # esp_eth: the RMII bring-up for boards whose config wires a PHY (ethStart).
    REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_eth app_update esp_https_ota
             mbedtls esp_partition joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
