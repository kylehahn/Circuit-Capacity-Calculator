cmake_minimum_required(VERSION 3.25)

project(circuit_capacity_calculator
    VERSION 0.1.0
    DESCRIPTION "Circuit capacity calculator with BEM solver and 3D layout editor"
    LANGUAGES CXX
)

# --- C++ standard ----------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Qt code generation ----------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# --- Default build type ----------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Diagnostics -----------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# --- Dependencies (resolved by vcpkg manifest) -----------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
find_package(Eigen3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinygltf CONFIG REQUIRED)
find_package(OpenMP)

# --- Targets ---------------------------------------------------------------
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "circuit_capacity_calculator ${PROJECT_VERSION}  (build type: ${CMAKE_BUILD_TYPE})")
message(STATUS "  Qt6        ${Qt6_VERSION}")
message(STATUS "  Eigen3     ${Eigen3_VERSION}")
message(STATUS "  OpenMP     ${OpenMP_CXX_FOUND}")
