idf_component_register(
    SRCS 
        "rf_module.cc"
        "rcswitch.cc"
        "tcswitch.cc"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        nvs_flash
)

