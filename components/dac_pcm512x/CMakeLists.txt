if(CONFIG_DAC_PCM512X)
    set(srcs "dac_pcm512x.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES dac
    PRIV_REQUIRES driver esp_driver_i2c board_utils
)
