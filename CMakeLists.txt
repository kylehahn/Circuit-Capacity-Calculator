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

# --- Optional compiler caching (sccache or ccache) ------------------------
# Auto-enabled if either is found on PATH; otherwise builds run normally.
# To install: `winget install Mozilla.sccache` (Win), `brew install sccache`
# (mac), distro package (Linux). For shared remote cache see CLAUDE.md.
find_program(CCC_COMPILER_LAUNCHER NAMES sccache ccache)
if(CCC_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCC_COMPILER_LAUNCHER}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCC_COMPILER_LAUNCHER}" CACHE FILEPATH "" FORCE)
    message(STATUS "  compiler-cache  ${CCC_COMPILER_LAUNCHER}")
else()
    # Clear any stale launcher value from a previous configure so a missing
    # cache executable doesn't break the build.
    unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
    unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
    message(STATUS "  compiler-cache  not found (install sccache/ccache for faster rebuilds)")
endif()

# --- Dependencies (resolved by vcpkg manifest) -----------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
find_package(Eigen3 CONFIG REQUIRED)
find_package(Clipper2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# tinygltf is header-only — no CMake config package, just an include path.
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h" REQUIRED)
find_package(OpenMP)

# --- Optional GPU (cuSOLVER) for the dense LU ------------------------------
# CUDA Toolkit must be installed separately; vcpkg does not ship it.
# Pass -DCCC_ENABLE_CUDA=ON at configure to opt in.
option(CCC_ENABLE_CUDA "Enable CUDA cuSOLVER GPU solve path" OFF)
if(CCC_ENABLE_CUDA)
    find_package(CUDAToolkit)
    if(CUDAToolkit_FOUND)
        message(STATUS "  CUDA       ${CUDAToolkit_VERSION} (cuSOLVER available)")
        set(CCC_HAS_CUDA TRUE)
    else()
        message(WARNING "CCC_ENABLE_CUDA=ON but CUDA Toolkit not found; "
                        "falling back to CPU-only build.")
        set(CCC_HAS_CUDA FALSE)
    endif()
else()
    set(CCC_HAS_CUDA FALSE)
endif()

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
