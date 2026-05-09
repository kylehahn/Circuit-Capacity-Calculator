find_package(Catch2 3 REQUIRED)

add_executable(unit_tests
    test_smoke.cpp
    test_model.cpp
    test_jsonio.cpp
)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    ccc_core
    ccc_io
)

include(Catch)
catch_discover_tests(unit_tests)
