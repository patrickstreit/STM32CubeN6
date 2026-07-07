cmake_minimum_required(VERSION 3.22)
include(ExternalProject)

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Set the project name
set(CMAKE_PROJECT_NAME VENC_SDCard_ThreadX)

# Core project settings
project(${CMAKE_PROJECT_NAME})

#
# Dual-image boot layout (LRUN): the FSBL and the Appli are built as two
# independent firmware images via ExternalProject_Add and signed separately.
#   - FSBL  : stored in external XSPI2 NOR flash @ 0x70000000, copied to AXISRAM2
#   - Appli : stored in external XSPI2 NOR flash @ 0x70100000, copied to AXISRAM
#
set(ST_MULTICONTEXT BOOT_FLASH CACHE STRING "Type of multi-context")

#-----------------------Build FSBL Project-----------------------#
if((${BUILD_CONTEXT} MATCHES .*FSBL.*) OR (NOT DEFINED BUILD_CONTEXT))
    message("   Build context: " FSBL)
    ExternalProject_Add(VENC_SDCard_ThreadX_FSBL
        BINARY_DIR                  ${CMAKE_SOURCE_DIR}/FSBL/build
        SOURCE_DIR                  ${PROJECT_SOURCE_DIR}/FSBL
        PREFIX                      FSBL
        CONFIGURE_HANDLED_BY_BUILD  true
        INSTALL_COMMAND             ""
        CMAKE_ARGS                  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON  -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_ALWAYS                true
    )

    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${CMAKE_SOURCE_DIR}/FSBL/build")
    set(ST_BOOT_FLASH_FSBL_PROJECT_BUILD_TARGET ${CMAKE_SOURCE_DIR}/FSBL/build/VENC_SDCard_ThreadX_FSBL${CMAKE_EXECUTABLE_SUFFIX_CXX} CACHE FILEPATH "Path to fsbl project target")
endif()

#-----------------------Build Appli Project-----------------------#
if((${BUILD_CONTEXT} MATCHES .*Appli.*) OR (NOT DEFINED BUILD_CONTEXT))
    message("   Build context: " Appli)
    ExternalProject_Add(VENC_SDCard_ThreadX_Appli
        BINARY_DIR                  ${CMAKE_SOURCE_DIR}/Appli/build
        SOURCE_DIR                  ${PROJECT_SOURCE_DIR}/Appli
        PREFIX                      Appli
        CONFIGURE_HANDLED_BY_BUILD  true
        INSTALL_COMMAND             ""
        CMAKE_ARGS                  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON  -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_ALWAYS                true
    )

    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${CMAKE_SOURCE_DIR}/Appli/build")
    set(ST_BOOT_FLASH_APPLI_PROJECT_BUILD_TARGET ${CMAKE_SOURCE_DIR}/Appli/build/VENC_SDCard_ThreadX_Appli${CMAKE_EXECUTABLE_SUFFIX_CXX} CACHE FILEPATH "Path to appli project target")
endif()
