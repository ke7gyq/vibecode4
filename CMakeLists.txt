

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================





cmake_minimum_required(VERSION 3.12)

# Get absolute paths to dependencies in parent directory
get_filename_component(PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FREERTOS_KERNEL_PATH "${PARENT_DIR}/FreeRTOS-Kernel_real")
set(LVGL_PATH "${PARENT_DIR}/lvgl")
set(PICO_SDK_PATH "${PARENT_DIR}/pico-sdk")
set(CMSIS_DSP_PATH "${PARENT_DIR}/CMSIS-DSP")
set(CMSIS_6_PATH "${PARENT_DIR}/CMSIS_6")

# Set platform and compiler for RP2350
set(PICO_PLATFORM rp2350)
set(PICO_COMPILER pico_arm_cortex_m33_gcc)
set(PICO_BOARD pico2_w)



# Include the pico-sdk import before project()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Include the pre-load toolchain to set up the correct compiler
include(${PICO_SDK_PATH}/cmake/pico_pre_load_toolchain.cmake)

# Project definition
project(vibecode4 C CXX ASM)

# Set C and C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Ensure ASM files are processed correctly
enable_language(ASM)

# Initialize pico SDK
pico_sdk_init()

# OPTIMIZATION: Per-file optimization flags
# Apply -O0 to FreeRTOS synchronization files (queues, semaphores, volatile state)
# Apply -O3 to DSP/FFT/filtering routines (critical performance path)
# Memory barriers refactored to use FreeRTOS queues (no explicit __atomic_thread_fence)

# Default to -Og (optimize for debugging)
add_compile_options(-Og -g)

# Include FreeRTOS Kernel for RP2350
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

# Configure CMSIS_6 include paths for CMSIS-DSP dependency
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${CMSIS_6_PATH}/CMSIS/Core/Include")

# Add CMSIS-DSP as a subdirectory for FFT and signal processing
add_subdirectory(${CMSIS_DSP_PATH} ${CMAKE_BINARY_DIR}/CMSIS-DSP)

# =====================================================================
# Generate configuration headers from configuration.json
# =====================================================================
set(CONFIGURATION_JSON "${CMAKE_CURRENT_SOURCE_DIR}/configuration.json")
set(CONFIG_CONSTANTS_H "${CMAKE_CURRENT_BINARY_DIR}/generated/config_constants.h")
set(LUT_PARAMS_H "${CMAKE_CURRENT_BINARY_DIR}/generated/LUT_Params.h")

# Ensure generated directory exists
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Generate config_constants.h from configuration.json
add_custom_command(
    OUTPUT "${CONFIG_CONSTANTS_H}"
    COMMAND ${CMAKE_COMMAND} -E echo "Generating config_constants.h from configuration.json..."
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_headers.py"
            "${CONFIGURATION_JSON}"
            "${CONFIG_CONSTANTS_H}"
    DEPENDS "${CONFIGURATION_JSON}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_headers.py"
    COMMENT "Generating system configuration constants"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# Generate LUT_Params.h from configuration.json
add_custom_command(
    OUTPUT "${LUT_PARAMS_H}"
    COMMAND ${CMAKE_COMMAND} -E echo "Generating LUT_Params.h from configuration.json..."
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/utils/generate_lut.py"
            "${CONFIGURATION_JSON}"
            "${LUT_PARAMS_H}"
    DEPENDS "${CONFIGURATION_JSON}" "${CMAKE_CURRENT_SOURCE_DIR}/utils/generate_lut.py"
    COMMENT "Generating OpenPDM2PCM LUT lookup table"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# Add custom targets to track header generation
add_custom_target(generate_config_headers ALL DEPENDS "${CONFIG_CONSTANTS_H}" "${LUT_PARAMS_H}")

# Source files
set(SOURCES
    src/main.c
    src/st7789.c
    src/trustzone_stubs.c
    src/parser.c
    src/widgets.c
    src/waterfall.c
    src/spectrogram.c
    src/microphone.c
    src/audio_hub.c
    src/audio_dsp_example.c
    src/OpenPDMFilter.c
    src/network.c
    src/udp_audio_server.c
    src/fault_handler.c
)

# Create executable for vibecode4
add_executable(vibecode4 ${SOURCES})

# Make vibecode4 depend on header generation from configuration.json
add_dependencies(vibecode4 generate_config_headers)

# Include directories for main executable
target_include_directories(vibecode4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${LVGL_PATH}
    ${CMSIS_DSP_PATH}/Include
    ${CMSIS_6_PATH}/CMSIS/Core/Include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_gpio/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_pio/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_dma/include
)

# =====================================================================
# Per-file optimization flags
# =====================================================================
# Files with FreeRTOS synchronization (queues, semaphores) use -O0
# to preserve volatile semantics and queue operations correctly
# Note: Explicit memory barriers (__atomic_thread_fence) removed
# in favor of FreeRTOS queue-based synchronization
set_source_files_properties(
    src/microphone.c
    src/fault_handler.c
    PROPERTIES
    COMPILE_FLAGS "-O0"
)

# spectrogram.c: FFT and waterfall processing - enable aggressive optimization
set_source_files_properties(
    src/spectrogram.c
    PROPERTIES
    COMPILE_FLAGS "-O3 -g"
)

# FFT and audio DSP routines - enable aggressive optimization
set_source_files_properties(
    src/audio_dsp_example.c
    src/OpenPDMFilter.c
    PROPERTIES
    COMPILE_FLAGS "-O3"
)


# LVGL library (stage 1: core LVGL)
file(GLOB_RECURSE LVGL_CORE_SOURCES 
    "${LVGL_PATH}/src/*.c"
)
add_library(lvgl STATIC ${LVGL_CORE_SOURCES})
target_include_directories(lvgl PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LVGL_PATH}
)

# Generate PIO header from the .pio file
pico_generate_pio_header(vibecode4 ${CMAKE_CURRENT_LIST_DIR}/src/st7789_lcd.pio)
pico_generate_pio_header(vibecode4 ${CMAKE_CURRENT_LIST_DIR}/src/gpio_toggle_test.pio)
pico_generate_pio_header(vibecode4 ${CMAKE_CURRENT_LIST_DIR}/src/pdm_clock.pio)




target_compile_definitions(vibecode4 PRIVATE
    PICO_DEFAULT_UART_TX_PIN=16
    PICO_DEFAULT_UART_RX_PIN=17
    CYW43_LWIP=1
    PICO_CYW43_LOGGING_ENABLED=0
    USE_CONST_LUT=1
)



# Link libraries to main executable
target_link_libraries(vibecode4
    pico_stdlib
    pico_multicore
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_uart
    lvgl
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    CMSISDSP
    pico_cyw43_arch_lwip_poll
    pico_lwip
    pico_status_led

)




# Disable both USB and UART output (not needed for graphics-only app)
pico_enable_stdio_usb(vibecode4 0)
pico_enable_stdio_uart(vibecode4 1)

# Create map/bin/hex/uf2 file in addition to ELF
pico_add_extra_outputs(vibecode4)
