# the 307,200 B framebuffer travels inside the app image; the runtime symbol is
# _binary_photo64_640x480_bin_start (main.c declares it with extern asm()).
idf_component_register(SRCS "main.c" "live.c"
                       INCLUDE_DIRS "."
                       REQUIRES mbedtls esp_wifi nvs_flash esp_netif esp_event driver fatfs sdmmc vfs esp_lcd)

