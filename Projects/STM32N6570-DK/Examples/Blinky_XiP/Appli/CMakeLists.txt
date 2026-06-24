cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

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

#Linker options
set(STM32_LINKER_SCRIPT STM32N657XX_ROMxspi2.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME Blinky_XiP_Appli)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")


# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()

# Specify the output ELF file
set(TARGET_ELF "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf")

# Command to generate .bin file
add_custom_command(
 TARGET ${CMAKE_PROJECT_NAME}
 POST_BUILD
 COMMAND ${CMAKE_OBJCOPY} -O binary ${TARGET_ELF} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
 COMMENT "Generating .bin file"
)
# Define the paths to the STM32 Signing Tool and the input/output files
set(SIGNING_TOOL "C:/Program Files/ST/STM32CubeProgrammer_2.20.0/bin/STM32_SigningTool_CLI.exe")
# Define the input and output files for Appli
set(APPLI_INPUT "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
set(APPLI_OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.bin")
# Add a post-build step to sign the Appli binary
add_custom_command(
 TARGET ${CMAKE_PROJECT_NAME}
 POST_BUILD
 # Step 1: Create a file with 'y'
 COMMAND ${CMAKE_COMMAND} -E echo y > "${CMAKE_BINARY_DIR}/input.txt"
 # Step 2: Run the signing tool, redirecting input from the file
 # from STM32CubeProgrammer version 2.21.0 a new parameter is needed "-align" Change the command below to use the new parameter if you are using a version >= 2.21.0
 # COMMAND "${SIGNING_TOOL}" -bin "${APPLI_INPUT}" -nk -of 0x80000000 -t fsbl -o "${APPLI_OUTPUT}" -hv 2.3 -align -dump "${APPLI_OUTPUT}" < "${CMAKE_BINARY_DIR}/input.txt"
 COMMAND "${SIGNING_TOOL}" -bin "${APPLI_INPUT}" -nk -of 0x80000000 -t fsbl -o "${APPLI_OUTPUT}" -hv 2.3 -dump "${APPLI_OUTPUT}" < "${CMAKE_BINARY_DIR}/input.txt"

 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
 COMMENT "Signing Appli binary: ${APPLI_INPUT} -> ${APPLI_OUTPUT}"
)

# Convert the signed Appli .bin back to .elf
add_custom_command(
TARGET ${CMAKE_PROJECT_NAME}
POST_BUILD
COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --change-addresses=0x70100000 ${APPLI_OUTPUT} ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-trusted.elf
COMMENT "Converting signed .bin back to .elf in address [0x70100000]"
)

