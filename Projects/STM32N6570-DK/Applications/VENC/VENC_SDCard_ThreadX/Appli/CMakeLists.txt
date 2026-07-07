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
# Symbols (macros) - mirrors STM32CubeIDE Appli .cproject
# --------------------------------------------------------------------------
set(APP_Defines
    STM32N6xx
    STM32
    STM32N657xx
    TX_SINGLE_MODE_SECURE
    $<$<CONFIG:Debug>:DEBUG>
)

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
    ${REPO_ROOT}/Middlewares/ST/filex/common/inc
    ${REPO_ROOT}/Middlewares/ST/filex/ports/generic/inc
    ${REPO_ROOT}/Middlewares/ST/VideoEncoder_EWL
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/common
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/inc
    ${REPO_ROOT}/Middlewares/Third_Party/VideoEncoder/source/h264
)

# --------------------------------------------------------------------------
# Application sources
# --------------------------------------------------------------------------
set(APP_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/app_filex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/app_threadx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/dcmipp_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/fx_stm32_sd_driver_glue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/lcd_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sdcard_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32n6xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/SystemClock_Config_800MHz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/venc_h264_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/tx_initialize_low_level.S
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/Patch/stm32n6570_discovery_camera.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sysmem.c
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
target_compile_definitions(stm32_iface INTERFACE ${APP_Defines})

add_library(STM32_Drivers OBJECT ${STM32_Drivers_Src} ${STM32_BSP_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32_iface)

add_library(ThreadX OBJECT ${ThreadX_Src})
target_link_libraries(ThreadX PUBLIC stm32_iface)

add_library(FileX OBJECT ${FileX_Src})
target_link_libraries(FileX PUBLIC stm32_iface)

add_library(VideoEncoder OBJECT ${VideoEncoder_Src})
target_link_libraries(VideoEncoder PUBLIC stm32_iface)

add_library(ISP_Library OBJECT ${ISP_Src})
target_link_libraries(ISP_Library PUBLIC stm32_iface)

# Prebuilt evision libraries (GCC variants)
set(EVISION_LIB_DIR ${REPO_ROOT}/Middlewares/ST/STM32_ISP_Library/evision/Lib)

# --------------------------------------------------------------------------
# Executable wiring
# --------------------------------------------------------------------------
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

# STM32 Signing Tool (hard-coded for STM32CubeProgrammer 2.20.0)
set(SIGNING_TOOL "C:/Program Files/ST/STM32CubeProgrammer_2.20.0/bin/STM32_SigningTool_CLI.exe")
set(APPLI_INPUT  "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
set(APPLI_OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.bin")

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo y > input.txt
    # From STM32CubeProgrammer >= 2.21.0 add the "-align" parameter to the command below.
    COMMAND "${SIGNING_TOOL}" -bin "${APPLI_INPUT}" -nk -of 0x80000000 -t fsbl -o "${APPLI_OUTPUT}" -hv 2.3 -dump "${APPLI_OUTPUT}" < input.txt
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Signing Appli binary: ${APPLI_INPUT} -> ${APPLI_OUTPUT}"
)

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --change-addresses=0x70100000 ${APPLI_OUTPUT} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.elf
    COMMENT "Converting signed Appli .bin back to .elf at address [0x70100000]"
)
