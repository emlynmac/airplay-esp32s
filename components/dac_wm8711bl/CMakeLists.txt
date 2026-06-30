if(CONFIG_DAC_WM8711BL)
    set(srcs "dac_wm8711bl.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES dac
    PRIV_REQUIRES driver esp_driver_i2c
)
