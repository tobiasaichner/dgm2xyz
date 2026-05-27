cmake_minimum_required(VERSION 3.20)

project(dgm2xyz VERSION 0.1.0 LANGUAGES CXX)

option(DGM2XYZ_BUILD_TESTS "Build dgm2xyz tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(dgm2xyz WIN32
  src/app_win32/main.cpp
)
target_link_libraries(dgm2xyz PRIVATE converter_core cad_dxf comctl32)
target_link_libraries(dgm2xyz PRIVATE comdlg32 shell32)
target_compile_definitions(dgm2xyz PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(DGM2XYZ_BUILD_TESTS)
  enable_testing()

  add_executable(dgm2xyz_tests
    tests/core_tests.cpp
  )
  target_link_libraries(dgm2xyz_tests PRIVATE converter_core cad_dxf)
  add_test(NAME dgm2xyz_tests COMMAND dgm2xyz_tests)
endif()
