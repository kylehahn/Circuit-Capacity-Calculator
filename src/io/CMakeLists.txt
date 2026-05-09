add_library(ccc_io STATIC
    JsonIo.cpp
)

target_include_directories(ccc_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ccc_io PUBLIC cxx_std_20)
target_link_libraries(ccc_io
    PUBLIC  ccc_core
            nlohmann_json::nlohmann_json
)
