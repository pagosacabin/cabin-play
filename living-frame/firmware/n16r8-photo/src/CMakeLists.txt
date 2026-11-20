# the 307,200 B framebuffer travels inside the app image; the runtime symbol is
# _binary_photo64_640x480_bin_start (main.c declares it with extern asm()).
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES mbedtls)

