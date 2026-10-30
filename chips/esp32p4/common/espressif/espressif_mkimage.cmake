# ##############################################################################
# Generate the ESP32-P4 Simple Boot image (nuttx.bin) from the linked ELF.
#
# Mirrors the Make path:
#   esptool --chip <chip> elf2image --ram-only-header -fs <size> \
#           -fm <mode> -ff <freq> -o nuttx.bin nuttx
# ##############################################################################

if(NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "BINARY_DIR is required")
endif()

set(CONFIG_FILE "${BINARY_DIR}/.config")
if(NOT EXISTS "${CONFIG_FILE}")
  message(FATAL_ERROR "Missing ${CONFIG_FILE}")
endif()

file(STRINGS "${CONFIG_FILE}" CONFIG_LINES)

function(config_value name default out)
  set(value "${default}")
  foreach(line IN LISTS CONFIG_LINES)
    if(line MATCHES "^${name}=(.*)$")
      set(value "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  string(REPLACE "\"" "" value "${value}")
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

config_value(CONFIG_ESPRESSIF_CHIP_SERIES "esp32p4" CHIP)
config_value(CONFIG_ESPRESSIF_FLASH_SIZE "16MB" FLASH_SIZE)
config_value(CONFIG_ESPRESSIF_FLASH_MODE "dio" FLASH_MODE)
config_value(CONFIG_ESPRESSIF_FLASH_FREQ "80m" FLASH_FREQ)

# Flash size/mode/freq are expressed as boolean selects in Kconfig; derive the
# values the same way the Make image rule does.
if(EXISTS "${CONFIG_FILE}")
  file(STRINGS "${CONFIG_FILE}" FLASH_LINES REGEX "^CONFIG_ESPRESSIF_FLASH_(16M|8M|4M|2M|MODE_|FREQ_)")
  foreach(line IN LISTS FLASH_LINES)
    if(line MATCHES "^CONFIG_ESPRESSIF_FLASH_(16M|8M|4M|2M)=y$")
      set(FLASH_SIZE "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^CONFIG_ESPRESSIF_FLASH_MODE_([A-Z]+)=y$")
      string(TOLOWER "${CMAKE_MATCH_1}" FLASH_MODE)
    elseif(line MATCHES "^CONFIG_ESPRESSIF_FLASH_FREQ_([0-9]+M)=y$")
      string(TOLOWER "${CMAKE_MATCH_1}" FLASH_FREQ)
    endif()
  endforeach()
endif()

if(FLASH_SIZE MATCHES "^([0-9]+)M$")
  set(FLASH_SIZE "${CMAKE_MATCH_1}MB")
endif()

find_program(ESPTOOL_BIN NAMES esptool esptool.py)
if(NOT ESPTOOL_BIN)
  message(FATAL_ERROR "esptool not found in PATH")
endif()

set(ELF "${BINARY_DIR}/nuttx")
set(IMAGE "${BINARY_DIR}/nuttx.bin")

execute_process(
  COMMAND ${ESPTOOL_BIN} --chip ${CHIP} elf2image --ram-only-header
          -fs ${FLASH_SIZE} -fm ${FLASH_MODE} -ff ${FLASH_FREQ}
          -o ${IMAGE} ${ELF}
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE OUTPUT
  ERROR_VARIABLE ERROR)

if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "elf2image failed:\n${OUTPUT}\n${ERROR}")
endif()

message(STATUS "Generated ${IMAGE} (${FLASH_SIZE}, ${FLASH_MODE}, ${FLASH_FREQ})")
