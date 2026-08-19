idf_component_register(
    SRCS "main.c" "avatar_display.c" "trapdoor.c" "ghost_ble.c"
    INCLUDE_DIRS "."
    REQUIRES bt driver esp_lcd esp_http_server esp_netif esp_timer esp_wifi lwip nvs_flash
    EMBED_TXTFILES "portal.html"
)

target_add_binary_data(${COMPONENT_LIB} "avatar_rgb565.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "meme_rgb565.bin" BINARY)
