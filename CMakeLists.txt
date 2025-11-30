# RF Module Configuration Options
# These can be set by the main project via CMake or Kconfig.projbuild
# Default values maintain backward compatibility (all features enabled)

option(RF_MODULE_ENABLE_FLASH_STORAGE "Enable Flash Storage (NVS)" ON)
option(RF_MODULE_ENABLE_433MHZ "Enable 433MHz Frequency Support" ON)
option(RF_MODULE_ENABLE_315MHZ "Enable 315MHz Frequency Support" ON)
option(RF_MODULE_ENABLE_MCP_TOOLS "Enable MCP Tools" ON)

# Configuration parameters with defaults
if(NOT DEFINED RF_MODULE_MAX_FLASH_SIGNALS)
    set(RF_MODULE_MAX_FLASH_SIGNALS 10)
endif()

if(NOT DEFINED RF_MODULE_LOG_LEVEL)
    set(RF_MODULE_LOG_LEVEL 3)
endif()

# Set compile definitions based on CMake options
# These will override defaults in rf_module_config.h
# Note: add_compile_definitions() applies to all targets in this directory
if(RF_MODULE_ENABLE_FLASH_STORAGE)
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE=1)
else()
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_FLASH_STORAGE=0)
endif()

if(RF_MODULE_ENABLE_433MHZ)
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_433MHZ=1)
else()
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_433MHZ=0)
endif()

if(RF_MODULE_ENABLE_315MHZ)
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_315MHZ=1)
else()
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_315MHZ=0)
endif()

if(RF_MODULE_ENABLE_MCP_TOOLS)
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_MCP_TOOLS=1)
else()
    add_compile_definitions(CONFIG_RF_MODULE_ENABLE_MCP_TOOLS=0)
endif()

add_compile_definitions(
    CONFIG_RF_MODULE_MAX_FLASH_SIGNALS=${RF_MODULE_MAX_FLASH_SIGNALS}
    CONFIG_RF_MODULE_LOG_LEVEL=${RF_MODULE_LOG_LEVEL}
)

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
)

