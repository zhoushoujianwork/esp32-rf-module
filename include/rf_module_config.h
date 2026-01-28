#ifndef RF_MODULE_CONFIG_H
#define RF_MODULE_CONFIG_H

/**
 * RF Module Configuration Header
 * 
 * This file provides default configuration values for the RF module component.
 * Configuration can be overridden by:
 * 1. CMake options in the main project's CMakeLists.txt
 * 2. Compile definitions set by the main project
 * 3. Main project's Kconfig.projbuild (which sets CMake variables)
 * 
 * All values default to enabled/backward compatible settings.
 */

// Frequency Support Configuration
#ifndef CONFIG_RF_MODULE_ENABLE_433MHZ
#define CONFIG_RF_MODULE_ENABLE_433MHZ 1
#endif

#ifndef CONFIG_RF_MODULE_ENABLE_315MHZ
#define CONFIG_RF_MODULE_ENABLE_315MHZ 1
#endif

// CC1101 Transceiver Configuration (optional, for feature/cc1101 branch)
#ifndef CONFIG_RF_MODULE_ENABLE_CC1101
#define CONFIG_RF_MODULE_ENABLE_CC1101 0
#endif

#ifndef CONFIG_RF_MODULE_MAX_STORED_SIGNALS
#define CONFIG_RF_MODULE_MAX_STORED_SIGNALS 10
#endif

// SD Storage Configuration (replaces NVS when enabled)
#ifndef CONFIG_RF_MODULE_ENABLE_SD_STORAGE
#define CONFIG_RF_MODULE_ENABLE_SD_STORAGE 0
#endif

// MCP Tools Configuration
#ifndef CONFIG_RF_MODULE_ENABLE_MCP_TOOLS
#define CONFIG_RF_MODULE_ENABLE_MCP_TOOLS 1
#endif

// Log Level Configuration
// 0 = None, 1 = Error, 2 = Warning, 3 = Info, 4 = Debug, 5 = Verbose
#ifndef CONFIG_RF_MODULE_LOG_LEVEL
#define CONFIG_RF_MODULE_LOG_LEVEL 3
#endif

#endif // RF_MODULE_CONFIG_H


