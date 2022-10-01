cmake_minimum_required(VERSION 3.8)

# Version setup
set(OPEN_LIBINPUT_VERSION_MAJOR "0")
set(OPEN_LIBINPUT_VERSION_MINOR "0")
set(OPEN_LIBINPUT_VERSION_PATCH "0")
set(OPEN_LIBINPUT_VERSION "${OPEN_LIBINPUT_VERSION_MAJOR}.${OPEN_LIBINPUT_VERSION_MINOR}.${OPEN_LIBINPUT_VERSION_PATCH}")

project(input
        VERSION ${OPEN_LIBINPUT_VERSION}
        DESCRIPTION "OpenBSD libinput "
        HOMEPAGE_URL "https://github.com/sizeofvoid/libinput"
        LANGUAGES C)

include(GNUInstallDirs)

option(ENABLE_SHARED_LIBS "Enable build and install shared libraries" ON)
option(ENABLE_STATIC_LIBS "Enable build and install static libraries" OFF)

## Set the build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release"
      CACHE STRING "Build configuration 'Release' or 'Debug'."
      FORCE)
endif()

set(OPEN_LIBINPUT_HEADER
    libinput-private.h
    libinput-util.h
    libinput.h)

set(OPEN_LIBINPUT_SOURCES
    libinput-util.c
    libinput.c)


# Offer the user the choice of overriding the installation directories
set(INSTALL_LIB_DIR lib CACHE PATH "Installation directory for libraries")
set(INSTALL_INCLUDE_DIR include CACHE PATH
  "Installation directory for header files")

# Make relative paths absolute (needed later on)
foreach(p LIB BIN INCLUDE CMAKE)
  set(var INSTALL_${p}_DIR)
  if(NOT IS_ABSOLUTE "${${var}}")
    set(${var} "${CMAKE_INSTALL_PREFIX}/${${var}}")
  endif()
endforeach()

foreach (TYPE IN ITEMS STATIC SHARED)
  if (ENABLE_${TYPE}_LIBS)
    string (TOLOWER "${TYPE}" type)
    add_library(input-${type} ${TYPE} ${OPEN_LIBINPUT_SOURCES} ${OPEN_LIBINPUT_HEADER})

    set_target_properties(input-${type} PROPERTIES
        VERSION ${OPEN_LIBINPUT_VERSION}
        SOVERSION ${OPEN_LIBINPUT_VERSION_MAJOR})

    target_include_directories(input-${type} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/input/>)

    # XXX readlink
    #set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -pedantic " CACHE STRING "Set C++ Compiler Flags" FORCE)
    #target_compile_options(input-${type} PRIVATE "-Wall")
    #target_compile_options(input-${type} PRIVATE "-pipe")
    #   target_compile_options(input-${type} PRIVATE "-fexceptions")

  #target_compile_options(input-${type} PUBLIC
  #    $<$<CONFIG:RELEASE>:${}>
  #    $<$<CONFIG:DEBUG:${}>)
  endif()
endforeach()

# Export the package for use from the build-tree
# (this registers the build-tree with a global CMake-registry)
export(PACKAGE input)

# Create the inputConfig.cmake and inputConfigVersion files
file(RELATIVE_PATH REL_INCLUDE_DIR "${INSTALL_CMAKE_DIR}"
   "${INSTALL_INCLUDE_DIR}")

if(ENABLE_SHARED_LIBS)
  set_target_properties(input-shared PROPERTIES OUTPUT_NAME input)
  install(TARGETS input-shared
    RENAME input
    EXPORT inputTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(ENABLE_STATIC_LIBS)
    set_target_properties(input-static PROPERTIES OUTPUT_NAME input)
    install(TARGETS input-static
        RENAME input
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(FILES include/linux/freebsd/input-event-codes.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/freebsd)
# KDE KWin?
install(FILES include/linux/freebsd/input-event-codes.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/)
install(FILES include/linux/freebsd/input.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/freebsd)
install(FILES include/linux/linux/input-event-codes.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/linux)
install(FILES include/linux/linux/input.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/linux)
install(FILES include/linux/input.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linux/)
install(FILES ${OPEN_LIBINPUT_HEADER} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
