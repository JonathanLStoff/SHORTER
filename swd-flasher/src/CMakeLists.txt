# src/fw_images.c is generated from firmware/*.bin by scripts/gen_images.py (a PlatformIO
# pre-build script), so the controller images are compiled into the ESP32 image.
idf_component_register(
    SRCS main.c led.c esp_swd_io.c swd.c gd32.c fw_images.c mailbox.c control.c
    INCLUDE_DIRS .
    REQUIRES driver esp_driver_usb_serial_jtag esp_hw_support esp_rom log freertos
)
