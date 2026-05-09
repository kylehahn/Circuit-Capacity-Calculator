add_library(ccc_core STATIC
    Pad.cpp
    Model.cpp
)

target_include_directories(ccc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ccc_core PUBLIC cxx_std_20)
target_link_libraries(ccc_core PUBLIC Eigen3::Eigen)
