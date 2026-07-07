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
set(STM32_LINKER_SCRIPT STM32N657XX_AXISRAM2_fsbl.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME VENC_SDCard_ThreadX_FSBL)
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
# Symbols (macros) - mirrors STM32CubeIDE FSBL .cproject
# --------------------------------------------------------------------------
set(FSBL_Defines
    NO_OTP_FUSE
    STM32N657xx
    STM32N6
    STM32
    STM32N6xx
    $<$<CONFIG:Debug>:DEBUG>
)

# --------------------------------------------------------------------------
# Include paths
# --------------------------------------------------------------------------
set(FSBL_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/Inc
    ${REPO_ROOT}/Drivers/CMSIS/Include
    ${REPO_ROOT}/Drivers/CMSIS/Device/ST/STM32N6xx/Include
    ${REPO_ROOT}/Drivers/STM32N6xx_HAL_Driver/Inc
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/sal
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/boot
)

# --------------------------------------------------------------------------
# Application sources
# --------------------------------------------------------------------------
set(FSBL_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/extmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/stm32n6xx_hal_msp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/stm32n6xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/system_stm32n6xx_fsbl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Startup/startup_stm32n657xx_fsbl.s
)

# --------------------------------------------------------------------------
# STM32 HAL drivers (globbed; unused objects are dropped by --gc-sections)
# --------------------------------------------------------------------------
file(GLOB STM32_Drivers_Src ${REPO_ROOT}/Drivers/STM32N6xx_HAL_Driver/Src/*.c)
list(FILTER STM32_Drivers_Src EXCLUDE REGEX ".*_template\\.c$")

# --------------------------------------------------------------------------
# STM32 ExtMem Manager (LRUN boot variant)
# --------------------------------------------------------------------------
set(STM32_ExtMem_Manager_Src
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/stm32_extmem.c
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_lrun.c
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_data.c
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_driver.c
    ${REPO_ROOT}/Middlewares/ST/STM32_ExtMem_Manager/sal/stm32_sal_xspi.c
)

# --------------------------------------------------------------------------
# Interface library for includes and symbols
# --------------------------------------------------------------------------
add_library(stm32_iface INTERFACE)
target_include_directories(stm32_iface INTERFACE ${FSBL_Include_Dirs})
target_compile_definitions(stm32_iface INTERFACE ${FSBL_Defines})

add_library(STM32_Drivers OBJECT ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32_iface)

add_library(STM32_ExtMem_Manager OBJECT ${STM32_ExtMem_Manager_Src})
target_link_libraries(STM32_ExtMem_Manager PUBLIC stm32_iface)

# --------------------------------------------------------------------------
# Executable wiring
# --------------------------------------------------------------------------
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${FSBL_Application_Src})
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32_iface
    STM32_Drivers
    STM32_ExtMem_Manager
    ${TOOLCHAIN_LINK_LIBRARIES}
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# --------------------------------------------------------------------------
# Post-build: raw .bin -> sign (FSBL) -> signed .elf @ 0x70000000
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
set(FSBL_INPUT  "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
set(FSBL_OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.bin")

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo y > input.txt
    # From STM32CubeProgrammer >= 2.21.0 add the "-align" parameter to the command below.
    COMMAND "${SIGNING_TOOL}" -bin "${FSBL_INPUT}" -nk -of 0x80000000 -t fsbl -o "${FSBL_OUTPUT}" -hv 2.3 -dump "${FSBL_OUTPUT}" < input.txt
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Signing FSBL binary: ${FSBL_INPUT} -> ${FSBL_OUTPUT}"
)

add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --change-addresses=0x70000000 ${FSBL_OUTPUT} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.elf
    COMMENT "Converting signed FSBL .bin back to .elf at address [0x70000000]"
)
