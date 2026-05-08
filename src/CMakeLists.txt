qt_standard_project_setup()

qt_add_executable(circuit_capacity_calculator
    main.cpp
)

target_link_libraries(circuit_capacity_calculator PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    Eigen3::Eigen
    nlohmann_json::nlohmann_json
    tinygltf::tinygltf
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(circuit_capacity_calculator PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(circuit_capacity_calculator PRIVATE CCC_HAS_OPENMP=1)
endif()

set_target_properties(circuit_capacity_calculator PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Install rules — drop binaries in the standard places so windeployqt /
# macdeployqt / Linux AppImage scripts can find them.
include(GNUInstallDirs)
install(TARGETS circuit_capacity_calculator
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE  DESTINATION .
)
