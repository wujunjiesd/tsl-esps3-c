idf_component_register(
    SRCS "audio_prompt.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_codec_dev
    EMBED_FILES
        "assets/vin_saved.wav"
        "assets/connected.wav"
        "assets/reverse.wav"
        "assets/key_mode.wav"
)
