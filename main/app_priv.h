#pragma once

#if !defined(CONFIG_IDF_TARGET_ESP32) && \
    !defined(CONFIG_IDF_TARGET_ESP32S2) && \
    !defined(CONFIG_IDF_TARGET_ESP32C3) && \
    !defined(CONFIG_IDF_TARGET_ESP32S3)
#define CONFIG_IDF_TARGET_ESP32C3 1
#endif