set(COMPONENT_SRCDIRS
    "src"
    "src/aes"
    "src/hal"
    "src/lmic"
)
set(COMPONENT_ADD_INCLUDEDIRS
    "include" "src/hal"
)
set(COMPONENT_REQUIRES
    nvs_flash
    driver
    mbedtls
    esp_event
    esp_timer
    esp_hw_support
    "log"
)

register_component()

if (IDF_VER STRGREATER_EQUAL "v4.0")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-expansion-to-defined)
endif()
