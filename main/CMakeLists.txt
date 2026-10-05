idf_component_register(
    SRCS
        "app_main.cpp"
        "capsule_face.cpp"
        "catalog.cpp"
        "device_ui.cpp"
        "onboarding.cpp"
        "eye_engine.cpp"
        "raster.cpp"
        "eye_renderer.cpp"
        "imu_service.cpp"
        "preferences.cpp"
        "ota_service.cpp"
        "wifi_provisioning.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash driver esp_timer esp_mm waveshare__esp32_s3_touch_amoled_1_75c
             espressif__esp_lcd_touch
             esp_https_ota app_update esp_http_client esp-tls
             wifi_provisioning esp_wifi
)

# Device stores panel byte order in the framebuffer; host builds (Makefile) stay native.
target_compile_definitions(${COMPONENT_LIB} PRIVATE EYES_RGB565_SWAPPED)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wno-missing-field-initializers
    -fno-exceptions
    -fno-rtti
)
