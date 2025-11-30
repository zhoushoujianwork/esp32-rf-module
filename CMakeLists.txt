idf_component_register(
    SRCS 
        "src/rf_module.cc"
        "src/rcswitch.cc"
        "src/tcswitch.cc"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        nvs_flash
        esp_driver_gpio
    KCONFIG 
        "Kconfig"
)

