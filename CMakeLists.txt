cmake_minimum_required(VERSION 3.20)

project(dgm2xyz VERSION 0.1.0 LANGUAGES CXX)

option(DGM2XYZ_BUILD_TESTS "Build dgm2xyz tests" ON)
set(DGM2XYZ_LIBREDWG "AUTO" CACHE STRING "GNU LibreDWG support: AUTO, ON, or OFF")
set_property(CACHE DGM2XYZ_LIBREDWG PROPERTY STRINGS AUTO ON OFF)
if(NOT DGM2XYZ_LIBREDWG MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "DGM2XYZ_LIBREDWG must be AUTO, ON, or OFF")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_library(converter_core
  src/converter_core/Conversion.cpp
  src/converter_core/XyzWriter.cpp
)
target_include_directories(converter_core PUBLIC src/converter_core/include)

add_library(cad_dxf
  src/cad_dxf/DxfPointReader.cpp
)
target_include_directories(cad_dxf PUBLIC src/cad_dxf/include)
target_link_libraries(cad_dxf PUBLIC converter_core)

add_library(cad_dwg
  src/cad_dwg/DwgPointReader.cpp
)
target_include_directories(cad_dwg PUBLIC src/cad_dwg/include)
target_link_libraries(cad_dwg PUBLIC converter_core)

if(NOT DGM2XYZ_LIBREDWG STREQUAL "OFF")
  find_package(unofficial-libredwg CONFIG QUIET)
  if(TARGET unofficial::libredwg::libredwg)
    set(LIBREDWG_FOUND TRUE)
    set(LIBREDWG_TARGET unofficial::libredwg::libredwg)
  endif()

  if(NOT LIBREDWG_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBREDWG QUIET IMPORTED_TARGET libredwg)
    endif()

    if(NOT LIBREDWG_FOUND)
      find_path(LIBREDWG_INCLUDE_DIR NAMES dwg.h)
      find_library(LIBREDWG_LIBRARY NAMES redwg libredwg dwg)
      if(LIBREDWG_INCLUDE_DIR AND LIBREDWG_LIBRARY)
        set(LIBREDWG_FOUND TRUE)
        set(LIBREDWG_INCLUDE_DIRS "${LIBREDWG_INCLUDE_DIR}")
        set(LIBREDWG_LIBRARIES "${LIBREDWG_LIBRARY}")
      endif()
    endif()
  endif()

  if(LIBREDWG_FOUND)
    target_compile_definitions(cad_dwg PRIVATE DGM2XYZ_WITH_LIBREDWG)
    if(DEFINED LIBREDWG_TARGET)
      target_link_libraries(cad_dwg PRIVATE ${LIBREDWG_TARGET})
    elseif(TARGET PkgConfig::LIBREDWG)
      target_link_libraries(cad_dwg PRIVATE PkgConfig::LIBREDWG)
    else()
      target_include_directories(cad_dwg PRIVATE ${LIBREDWG_INCLUDE_DIRS})
      target_link_libraries(cad_dwg PRIVATE ${LIBREDWG_LIBRARIES})
    endif()
    message(STATUS "GNU LibreDWG support enabled")
  elseif(DGM2XYZ_LIBREDWG STREQUAL "ON")
    message(FATAL_ERROR "DGM2XYZ_LIBREDWG=ON but GNU LibreDWG was not found")
  else()
    message(STATUS "GNU LibreDWG not found; DWG support will report a runtime diagnostic")
  endif()
endif()

add_executable(dgm2xyz WIN32
  src/app_win32/main.cpp
)
target_link_libraries(dgm2xyz PRIVATE converter_core cad_dxf cad_dwg comctl32)
target_link_libraries(dgm2xyz PRIVATE comdlg32 shell32)
target_compile_definitions(dgm2xyz PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(DGM2XYZ_BUILD_TESTS)
  enable_testing()

  add_executable(dgm2xyz_tests
    tests/core_tests.cpp
  )
  target_link_libraries(dgm2xyz_tests PRIVATE converter_core cad_dxf cad_dwg)
  add_test(NAME dgm2xyz_tests COMMAND dgm2xyz_tests)
endif()
