cmake_minimum_required(VERSION 3.5.0 FATAL_ERROR) #CPACK_DEBIAN_<COMPONENT>_PACKAGE_NAME

find_package(IRODS 4.2.0 EXACT REQUIRED)

set(CMAKE_C_COMPILER ${IRODS_EXTERNALS_FULLPATH_CLANG}/bin/clang)
set(CMAKE_CXX_COMPILER ${IRODS_EXTERNALS_FULLPATH_CLANG}/bin/clang++)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")

project(irods-shareuf C CXX)

include(${IRODS_TARGETS_PATH})

set(IRODS_PLUGIN_VERSION_MAJOR "2")
set(IRODS_PLUGIN_VERSION_MINOR "0")
set(IRODS_PLUGIN_VERSION_PATCH "0")
set(IRODS_PLUGIN_VERSION "${IRODS_PLUGIN_VERSION_MAJOR}.${IRODS_PLUGIN_VERSION_MINOR}.${IRODS_PLUGIN_VERSION_PATCH}")

if (NOT CPACK_PACKAGING_INSTALL_PREFIX)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/" CACHE STRING "Package root path. \"/\" is correct for normal package builds.." FORCE)
  message(STATUS "Setting unspecified CPACK_PACKAGING_INSTALL_PREFIX to '${CPACK_PACKAGING_INSTALL_PREFIX}'. This is the correct setting for normal builds.")
endif()

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build {Debug, Release}." FORCE)
  message(STATUS "Setting unspecified CMAKE_BUILD_TYPE to '${CMAKE_BUILD_TYPE}'")
endif()

set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -stdlib=libc++ -Wl,-z,defs")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++ -Wl,-z,defs")
add_compile_options(-nostdinc++ -Wall -Wextra -Werror)
add_compile_options(-Wno-unused-parameter) # Until fix of irods_re_plugin.hpp
add_compile_options(-Wno-unneeded-internal-declaration) # Until fix of https://github.com/irods/irods/issues/3396
link_libraries(c++abi)
include_directories(${IRODS_EXTERNALS_FULLPATH_CLANG}/include/c++/v1)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_INSTALL_RPATH ${IRODS_EXTERNALS_FULLPATH_CLANG_RUNTIME}/lib)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

add_library(
  irods_shareuf_plugin
  MODULE
  ${CMAKE_SOURCE_DIR}/shareuf/libshareuf.cpp
  )
target_include_directories(
  irods_shareuf_plugin
  PRIVATE
  ${IRODS_INCLUDE_DIRS}
  ${IRODS_EXTERNALS_FULLPATH_BOOST}/include
  ${IRODS_EXTERNALS_FULLPATH_JANSSON}/include
  )
target_link_libraries(
  irods_shareuf_plugin
  PRIVATE
  irods_server
  irods_common
  ${IRODS_EXTERNALS_FULLPATH_BOOST}/lib/libboost_filesystem.so
  )
target_compile_definitions(irods_shareuf_plugin PRIVATE RODS_SERVER ${IRODS_COMPILE_DEFINITIONS} BOOST_SYSTEM_NO_DEPRECATED)
set_property(TARGET irods_shareuf_plugin PROPERTY CXX_STANDARD ${IRODS_CXX_STANDARD})
set_property(TARGET irods_shareuf_plugin PROPERTY OUTPUT_NAME shareuf)
install(
  TARGETS
  irods_shareuf_plugin
  LIBRARY
  DESTINATION ${IRODS_PLUGINS_DIRECTORY}/resources
  )

set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_PACKAGE_VERSION ${IRODS_PLUGIN_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${IRODS_PLUGIN_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${IRODS_PLUGIN_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${IRODS_PLUGIN_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "EuPathDB Bioinformatics Resource Center <help@eupathdb.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "iRODS shared unixfilesystem plugin")
set(CPACK_RPM_PACKAGE_DESCRIPTION 
"A derivative of the libunixfilesystem resource plugin that creates
directories and files with modes 0755 and 0644 on the resource
filesystem."
)
set(CPACK_PACKAGE_VENDOR "EuPathDB Bioinformatics Resource Center <help@eupathdb.org>")
SET(CPACK_PACKAGE_RELOCATABLE "false")

set(CPACK_DEB_COMPONENT_INSTALL OFF)
set(CPACK_DEBIAN_PACKAGE_SECTION "contrib/science")
set(CPACK_DEBIAN_COMPRESSION_TYPE "gzip")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "extra")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://irods.org")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_RPM_PACKAGE_NAME "irods-resource-plugin-shareuf-${IRODS_VERSION}")
set(CPACK_RPM_COMPONENT_INSTALL OFF)
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_LICENSE "BSD-3-Clause")
set(CPACK_RPM_PACKAGE_VENDOR "EuPathDB Bioinformatics Resource Center")
set(CPACK_RPM_PACKAGE_URL "https://github.com/EuPathDB/irods-libshareuf")
set(CPACK_RPM_PACKAGE_AUTOREQ 0)
set(CPACK_RPM_PACKAGE_AUTOPROV 0)
set(CPACK_RPM_FILE_NAME "${CPACK_RPM_PACKAGE_NAME}-${IRODS_PLUGIN_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}.x86_64.rpm")
set(CPACK_RPM_CHANGELOG_FILE "${CMAKE_CURRENT_LIST_DIR}/rpm_changelog.txt")

set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

set(CPACK_DEBIAN_PACKAGE_NAME "irods-resource-plugin-shareuf")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${IRODS_PACKAGE_DEPENDENCIES_STRING}, irods-runtime (= ${IRODS_CPACK_DEBIAN_PACKAGE_VERSION}), libc6")

if (IRODS_LINUX_DISTRIBUTION_NAME STREQUAL "centos" OR IRODS_LINUX_DISTRIBUTION_NAME STREQUAL "centos linux")
  set(CPACK_RPM_PACKAGE_REQUIRES "irods = ${IRODS_VERSION}")
elseif (IRODS_LINUX_DISTRIBUTION_NAME STREQUAL "opensuse")
  set(CPACK_RPM_PACKAGE_REQUIRES "${IRODS_PACKAGE_DEPENDENCIES_STRING}, irods-runtime = ${IRODS_VERSION}, libopenssl1_0_0")
endif()


if (NOT CPACK_GENERATOR)
  set(CPACK_GENERATOR ${IRODS_CPACK_GENERATOR} CACHE STRING "CPack generator to use, e.g. {DEB, RPM, TGZ}." FORCE)
  message(STATUS "Setting unspecified CPACK_GENERATOR to ${CPACK_GENERATOR}. This is the correct setting for normal builds.")
endif()

include(CPack)
