idf_component_register(
    SRCS
        "status_led.c"
        "main.c"
        "settings_store.c"
        "chat_service.c"
        "llm_inference.c"
        "https_server.c"
        "wifi_manager.c"
        "bt_manager.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "certs/servercert.pem"
        "certs/prvtkey.pem"
        "web/index.html"
    REQUIRES
        esp_driver_gpio
        esp_https_server
        esp_http_client
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        dns_server
        bt
        json
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
