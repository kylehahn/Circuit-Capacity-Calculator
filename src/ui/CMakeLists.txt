qt_add_library(ccc_ui STATIC
    Camera.cpp
    Viewport3D.cpp
    MainWindow.cpp
)
set_target_properties(ccc_ui PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC ON)

target_include_directories(ccc_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ccc_ui PUBLIC cxx_std_20)
target_link_libraries(ccc_ui
    PUBLIC  ccc_core
            ccc_io
            Qt6::Widgets
            Qt6::OpenGL
            Qt6::OpenGLWidgets
)
