# The embedded fallback raster (480,000 B for 800x600) travels inside the app
# image as photo64_data.h; the slideshow frames themselves live on the card.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd sdmmc fatfs spi_flash mbedtls esp_timer)
