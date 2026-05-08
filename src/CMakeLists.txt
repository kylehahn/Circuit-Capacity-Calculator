qt_standard_project_setup()

qt_add_executable(sensor3d
    main.cpp
)

target_link_libraries(sensor3d PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    Eigen3::Eigen
    nlohmann_json::nlohmann_json
    tinygltf::tinygltf
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(sensor3d PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(sensor3d PRIVATE SENSOR3D_HAS_OPENMP=1)
endif()

set_target_properties(sensor3d PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Install rules — drop binaries in the standard places so windeployqt /
# macdeployqt / Linux AppImage scripts can find them.
include(GNUInstallDirs)
install(TARGETS sensor3d
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE  DESTINATION .
)
