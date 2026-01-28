# RF Module Configuration Options
# These can be set by the main project via CMake or Kconfig.projbuild
# Default values maintain backward compatibility (all features enabled)

option(RF_MODULE_ENABLE_SD_STORAGE "Enable SD Card Signal Storage" OFF)
option(RF_MODULE_ENABLE_433MHZ "Enable 433MHz Frequency Support" ON)
option(RF_MODULE_ENABLE_315MHZ "Enable 315MHz Frequency Support" ON)
option(RF_MODULE_ENABLE_CC1101 "Enable CC1101 Transceiver Support" OFF)
option(RF_MODULE_ENABLE_MCP_TOOLS "Enable MCP Tools" ON)

# Configuration parameters with defaults
if(NOT DEFINED RF_MODULE_LOG_LEVEL)
    set(RF_MODULE_LOG_LEVEL 3)
endif()

set(RF_MODULE_SRCS "src/rf_module.cc" "src/rcswitch.cc" "src/tcswitch.cc")
set(RF_MODULE_REQUIRES esp_driver_gpio)

if(RF_MODULE_ENABLE_CC1101)
    list(APPEND RF_MODULE_SRCS "src/cc1101.cc")
    list(APPEND RF_MODULE_REQUIRES esp_driver_spi_master)
endif()

if(RF_MODULE_ENABLE_SD_STORAGE)
    list(APPEND RF_MODULE_REQUIRES fatfs)
endif()

idf_component_register(
    SRCS ${RF_MODULE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${RF_MODULE_REQUIRES}
)

# Set compile definitions based on CMake options
if(RF_MODULE_ENABLE_433MHZ)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_433MHZ=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_433MHZ=0)
endif()

if(RF_MODULE_ENABLE_315MHZ)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_315MHZ=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_315MHZ=0)
endif()

if(RF_MODULE_ENABLE_MCP_TOOLS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_MCP_TOOLS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_MCP_TOOLS=0)
endif()

if(RF_MODULE_ENABLE_CC1101)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_CC1101=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_CC1101=0)
endif()

if(RF_MODULE_ENABLE_SD_STORAGE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_SD_STORAGE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_RF_MODULE_ENABLE_SD_STORAGE=0)
endif()

if(NOT DEFINED RF_MODULE_MAX_STORED_SIGNALS)
    set(RF_MODULE_MAX_STORED_SIGNALS 10)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    CONFIG_RF_MODULE_MAX_STORED_SIGNALS=${RF_MODULE_MAX_STORED_SIGNALS}
    CONFIG_RF_MODULE_LOG_LEVEL=${RF_MODULE_LOG_LEVEL}
)

