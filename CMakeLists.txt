cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Core sources (all platforms)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

# Linux-only KMS/DMA-BUF capture sources
if(OS_LINUX)
  find_package(PkgConfig REQUIRED)

  # libdrm
  pkg_check_modules(Libdrm REQUIRED IMPORTED_TARGET libdrm)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::Libdrm)

  # EGL (for gs_texture_create_from_dmabuf internals — headers only needed
  # for drm_fourcc.h which ships with libdrm)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/kms-utils.c
      src/kms-ipc.c
      src/kmscap-source.c
  )

  # Install the NV12 conversion effect alongside the plugin data
  set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    OBS_MODULE_INSTALL_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data"
  )

  # Build the privileged helper daemon
  add_executable(obs-kmscap-helper src/kmscap-helper.c src/kms-utils.c src/kms-ipc.c)
  target_link_libraries(obs-kmscap-helper PRIVATE PkgConfig::Libdrm)
  target_compile_definitions(obs-kmscap-helper PRIVATE OS_LINUX KMSCAP_HELPER_DAEMON)
  install(TARGETS obs-kmscap-helper DESTINATION bin)

  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OS_LINUX)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
