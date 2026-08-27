cmake_minimum_required(VERSION 3.22)

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m55 -mfpu=fpv5-d16 -mfloat-abi=hard -mcmse " )

# Linker options
set(STM32_LINKER_SCRIPT STM32N657XX.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME VENC_SDCard_ThreadX_Appli)
project(${CMAKE_PROJECT_NAME})

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Path to the STM32CubeN6 repository root (6 levels up from this folder)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

# --------------------------------------------------------------------------
# Build variants
# --------------------------------------------------------------------------
# CSI_PROBE_MODE builds a minimal application that only brings up the
# DCMIPP/CSI-2 receiver: it characterises an unknown CSI-2 source and previews
# its virtual channels on the display. No encoder, no SD card, no FileX.
option(CSI_PROBE_MODE "Build the minimal CSI-2 probe / preview application" OFF)

# --------------------------------------------------------------------------
# Symbols (macros) - mirrors STM32CubeIDE Appli .cproject
# --------------------------------------------------------------------------
set(APP_Defines
    STM32N6xx
    STM32
    STM32N657xx
    TX_SINGLE_MODE_SECURE
    TX_EXECUTION_PROFILE_ENABLE
    TX_ENABLE_EVENT_TRACE
    FX_ENABLE_EVENT_TRACE
    $<$<CONFIG:Debug>:DEBUG>
)

if(CSI_PROBE_MODE)
    list(APPEND APP_Defines CSI_PROBE_MODE)
endif()

# PLAN.md M2 variant (b): let the encoder read its input picture from AXISRAM
# instead of the PSRAM capture buffer. The staging frame is 1.32 MB and only
# fits into the non-cacheable AXISRAM region next to a smaller bitstream ring,
# so this option shrinks that too - which is where the plan wants it anyway.
option(VENC_M2_AXISRAM_INPUT "Stage the encoder input frame in AXISRAM (PLAN.md M2)" OFF)

if(VENC_M2_AXISRAM_INPUT)
    list(APPEND APP_Defines VENC_M2_AXISRAM_INPUT)
    list(APPEND APP_Defines VENC_OUTPUT_BUFFER_SIZE=1000000U)
endif()

# AXISRAM is the scarce memory and the composite buffer wants to be there, so
# the question is whether the bitstream ring can be evicted to PSRAM without
# costing encode time. This option is how that gets measured (PLAN.md M2).
option(VENC_BITSTREAM_IN_PSRAM "Put the h264 bitstream ring in PSRAM instead of AXISRAM" OFF)

if(VENC_BITSTREAM_IN_PSRAM)
    list(APPEND APP_Defines VENC_BITSTREAM_IN_PSRAM)
endif()

# Both arms of that comparison use the same ring size, so the only difference
# between them is where the ring sits. 512 KB rather than the default 2 MB
# because PSRAM has only ~770 KB free until the linker script is corrected to
# the full 32 MB - and because the ring size is irrelevant to encode time.
option(VENC_SMALL_BITSTREAM_RING "Shrink the bitstream ring to 512 KB" OFF)

if(VENC_SMALL_BITSTREAM_RING)
    list(APPEND APP_Defines VENC_OUTPUT_BUFFER_SIZE=524288U)
endif()

# The composite geometry, as WIDTHxHEIGHT. The encode-time model used to be
# taken at 720p and scaled by macroblock count, which assumes the shape of the
# frame does not matter. After the composite was respecified from portrait to
# landscape that assumption is exactly what needs measuring, so the geometry
# became a build parameter and the encoder is configured at the real target
# size (PLAN.md M2-R).
#
# The DCMIPP downsizes the 1920x1080 source into it, so neither dimension may
# exceed the source - 1984 wide is not reachable and 1920x512 stands in for it
# at the same macroblock count.
set(VENC_GEOMETRY "" CACHE STRING "Encoder geometry as WIDTHxHEIGHT, empty for the config header default")

if(VENC_GEOMETRY)
    if(NOT VENC_GEOMETRY MATCHES "^([0-9]+)x([0-9]+)$")
        message(FATAL_ERROR "VENC_GEOMETRY must look like 1792x448, got '${VENC_GEOMETRY}'")
    endif()
    list(APPEND APP_Defines VENC_GEOMETRY_WIDTH=${CMAKE_MATCH_1}U)
    list(APPEND APP_Defines VENC_GEOMETRY_HEIGHT=${CMAKE_MATCH_2}U)
endif()

# --------------------------------------------------------------------------
# Include paths
# --------------------------------------------------------------------------
set(APP_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${REPO_ROOT}/Drivers/CMSIS/Include
    ${REPO_ROOT}/Drivers/CMSIS/Device/ST/STM32N6xx/Include
    ${REPO_ROOT}/Drivers/STM32N6xx_HAL_Driver/Inc
    ${REPO_ROOT}/Drivers/BSP/STM32N6570-DK
    ${REPO_ROOT}/Drivers/BSP/Components/Common
    ${REPO_ROOT}/Drivers/BSP/Components/imx335
    ${REPO_ROOT}/Middlewares/ST/STM32_ISP_Library/isp/Inc
    ${REPO_ROOT}/Middlewares/ST/STM32_ISP_Library/evision/Inc
    ${REPO_ROOT}/Middlewares/ST/threadx/common/inc
    ${REPO_ROOT}/Middlewares/ST/threadx/ports/cortex_m55/gnu/inc
    ${REPO_ROOT}/Middlewares/ST/threadx/utility/execution_profile_kit
    ${REPO_ROOT}/Middlewares/ST/filex/common/inc
    ${REPO_ROOT}/Middlewares/ST/filex/ports/generic/inc
    ${REPO_ROOT}/Middlewares/ST/VideoEncoder_EWL
)

# Marked SYSTEM below so warnings from vendor headers are not reported.
set(APP_System_Include_Dirs
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/common
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/inc
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/h264
)

# --------------------------------------------------------------------------
# Application sources
# --------------------------------------------------------------------------
set(APP_Probe_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/app_threadx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_probe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_preview.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_grab.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_phase.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_mux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/csi_probe_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/instrumentation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/SystemClock_Config_800MHz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/tx_initialize_low_level.S
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/Patch/stm32n6570_discovery_camera.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/AZURE_RTOS/App/app_azure_rtos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../FSBL/Src/system_stm32n6xx_fsbl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Startup/startup_stm32n657xx.s
)

set(APP_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/app_filex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/app_threadx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/debug_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/dcmipp_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/fx_stm32_sd_driver_glue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/instrumentation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/lcd_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sdcard_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/SystemClock_Config_800MHz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_bench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_h264_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/tx_initialize_low_level.S
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/Patch/stm32n6570_discovery_camera.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/frame_rb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/st_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/st_monitor_bitrate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/st_monitor_cpu_threadx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/time_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/AZURE_RTOS/App/app_azure_rtos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../FSBL/Src/system_stm32n6xx_fsbl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Startup/startup_stm32n657xx.s
)

# venc_h264_config.c needs relaxed brace-initializer warnings (per .cproject)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_h264_config.c
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-braces"
)

# --------------------------------------------------------------------------
# STM32 HAL drivers (globbed; unused objects are dropped by --gc-sections)
# --------------------------------------------------------------------------
file(GLOB STM32_Drivers_Src ${REPO_ROOT}/Drivers/STM32N6xx_HAL_Driver/Src/*.c)
list(FILTER STM32_Drivers_Src EXCLUDE REGEX ".*_template\\.c$")

# --------------------------------------------------------------------------
# Board support package (STM32N6570-DK + camera/PSRAM components)
# --------------------------------------------------------------------------
set(STM32_BSP_Src
    ${REPO_ROOT}/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery.c
    ${REPO_ROOT}/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_bus.c
    ${REPO_ROOT}/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_lcd.c
    ${REPO_ROOT}/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_xspi.c
    ${REPO_ROOT}/Drivers/BSP/Components/aps256xx/aps256xx.c
    ${REPO_ROOT}/Drivers/BSP/Components/imx335/imx335.c
    ${REPO_ROOT}/Drivers/BSP/Components/imx335/imx335_reg.c
)

# --------------------------------------------------------------------------
# Azure RTOS ThreadX kernel + Cortex-M55 GNU port
# (the application provides its own tx_initialize_low_level.S)
# --------------------------------------------------------------------------
file(GLOB ThreadX_Src ${REPO_ROOT}/Middlewares/ST/threadx/common/src/*.c)
file(GLOB ThreadX_Port_C ${REPO_ROOT}/Middlewares/ST/threadx/ports/cortex_m55/gnu/src/*.c)
file(GLOB ThreadX_Port_ASM ${REPO_ROOT}/Middlewares/ST/threadx/ports/cortex_m55/gnu/src/*.S)
list(FILTER ThreadX_Port_ASM EXCLUDE REGEX ".*tx_initialize_low_level\\.S$")
list(APPEND ThreadX_Src ${ThreadX_Port_C} ${ThreadX_Port_ASM})
list(APPEND ThreadX_Src ${REPO_ROOT}/Middlewares/ST/threadx/utility/execution_profile_kit/tx_execution_profile.c)

# --------------------------------------------------------------------------
# Azure RTOS FileX (FAT filesystem) + SD driver
# --------------------------------------------------------------------------
file(GLOB FileX_Src ${REPO_ROOT}/Middlewares/ST/filex/common/src/*.c)
list(APPEND FileX_Src ${REPO_ROOT}/Middlewares/ST/filex/common/drivers/fx_stm32_sd_driver.c)

# --------------------------------------------------------------------------
# H.264 video encoder (source build) + EWL (ThreadX memory/sync backend)
# --------------------------------------------------------------------------
file(GLOB VideoEncoder_Src
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/common/*.c
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/h264/*.c
)
list(APPEND VideoEncoder_Src ${REPO_ROOT}/Middlewares/ST/VideoEncoder_EWL/ewl_impl.c)

# --------------------------------------------------------------------------
# ISP library (camera image tuning; links prebuilt evision libraries below)
# --------------------------------------------------------------------------
file(GLOB ISP_Src ${REPO_ROOT}/Middlewares/ST/STM32_ISP_Library/isp/Src/*.c)
list(FILTER ISP_Src EXCLUDE REGEX ".*_template\\.c$")

# --------------------------------------------------------------------------
# Interface library for includes and symbols
# --------------------------------------------------------------------------
add_library(stm32_iface INTERFACE)
target_include_directories(stm32_iface INTERFACE ${APP_Include_Dirs})
target_include_directories(stm32_iface SYSTEM INTERFACE ${APP_System_Include_Dirs})
target_compile_definitions(stm32_iface INTERFACE ${APP_Defines})

add_library(STM32_Drivers OBJECT ${STM32_Drivers_Src} ${STM32_BSP_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32_iface)

add_library(ThreadX OBJECT ${ThreadX_Src})
target_link_libraries(ThreadX PUBLIC stm32_iface)

add_library(FileX OBJECT ${FileX_Src})
target_link_libraries(FileX PUBLIC stm32_iface)

add_library(VideoEncoder OBJECT ${VideoEncoder_Src})
target_link_libraries(VideoEncoder PUBLIC stm32_iface)
# Vendor code is not warning-clean; silence its known diagnostics here instead of
# patching sources that get overwritten on the next CubeN6 update.
target_compile_options(VideoEncoder PRIVATE
    -Wno-unused-function
    -Wno-format
    -Wno-int-in-bool-context
    -Wno-unused-variable
    -Wno-unused-but-set-variable
)

add_library(ISP_Library OBJECT ${ISP_Src})
target_link_libraries(ISP_Library PUBLIC stm32_iface)

# Prebuilt evision libraries (GCC variants)
set(EVISION_LIB_DIR ${REPO_ROOT}/Middlewares/ST/STM32_ISP_Library/evision/Lib)

# --------------------------------------------------------------------------
# Executable wiring
# --------------------------------------------------------------------------
if(CSI_PROBE_MODE)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${APP_Probe_Src})
    # The BSP camera driver still references the ISP library, so it stays linked
    # even though the probe never starts the ISP. FileX and the encoder are gone.
    target_link_libraries(${CMAKE_PROJECT_NAME}
        stm32_iface
        STM32_Drivers
        ThreadX
        ISP_Library
        ${EVISION_LIB_DIR}/libn6-evision-awb_gcc.a
        ${EVISION_LIB_DIR}/libn6-evision-st-ae_gcc.a
        ${TOOLCHAIN_LINK_LIBRARIES}
    )
else()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${APP_Application_Src})
    target_link_libraries(${CMAKE_PROJECT_NAME}
        stm32_iface
        STM32_Drivers
        ThreadX
        FileX
        VideoEncoder
        ISP_Library
        ${EVISION_LIB_DIR}/libn6-evision-awb_gcc.a
        ${EVISION_LIB_DIR}/libn6-evision-st-ae_gcc.a
        ${TOOLCHAIN_LINK_LIBRARIES}
    )
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# --------------------------------------------------------------------------
# Post-build: raw .bin -> sign (Appli) -> signed .elf @ 0x70100000
# --------------------------------------------------------------------------
# Resolve objcopy to a full path (derived from the compiler location) so the
# post-build steps work even when the toolchain bin dir is not on PATH.
get_filename_component(TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
set(CMAKE_OBJCOPY "${TOOLCHAIN_BIN_DIR}/arm-none-eabi-objcopy")

set(TARGET_ELF "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf")

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${TARGET_ELF} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating .bin file"
)

# STM32 Signing Tool Environment Check: Programmer in Path and version check
# From STM32CubeProgrammer >= 2.21.0 we have to add the "-align" parameter to the command, therefore we should check also the version of the signing tool and adapt the command accordingly.
if(NOT DEFINED ENV{STM32_PRG_PATH})
    message(FATAL_ERROR "Environment variable STM32_PRG_PATH is not set. Please set it to the STM32CubeProgrammer installation path.")
endif()

set(SIGNING_TOOL "$ENV{STM32_PRG_PATH}/STM32_SigningTool_CLI.exe")
set(APPLI_INPUT  "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
set(APPLI_OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.bin")

execute_process(
    COMMAND "${SIGNING_TOOL}" --version
    OUTPUT_VARIABLE SIGNING_TOOL_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    )

if (SIGNING_TOOL_VERSION_OUTPUT STREQUAL "")
    message(FATAL_ERROR "Failed to get Signing Tool version. Please ensure STM32_SigningTool_CLI.exe is in the path specified by STM32_PRG_PATH.")
endif()

string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
       SIGNING_TOOL_VERSION
       "${SIGNING_TOOL_VERSION_OUTPUT}")

message(STATUS "Signing Tool version: ${SIGNING_TOOL_VERSION}")

If("${SIGNING_TOOL_VERSION}" VERSION_LESS "2.21.0")
    set(SIGNING_ARGS
        -bin ${APPLI_INPUT}
        -nk
        -of 0x80000000
        -t fsbl
        -o ${APPLI_OUTPUT}
        -hv 2.3
        -dump ${APPLI_OUTPUT}
        -s # silently overwrite existing output file
    )
else()
    set(SIGNING_ARGS
        -bin ${APPLI_INPUT}
        -nk
        -of 0x80000000
        -t fsbl
        -o ${APPLI_OUTPUT}
        -hv 2.3
        -align # From STM32CubeProgrammer >= 2.21.0 we have to add the "-align" parameter
        -dump ${APPLI_OUTPUT}
        -s # silently overwrite existing output file
    )
endif()

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${SIGNING_TOOL} ${SIGNING_ARGS}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Signing Appli binary: ${APPLI_INPUT} -> ${APPLI_OUTPUT}"
)

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --change-addresses=0x70100000 ${APPLI_OUTPUT} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.elf
    COMMENT "Converting signed Appli .bin back to .elf at address [0x70100000]"
)
