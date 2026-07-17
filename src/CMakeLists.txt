qt_standard_project_setup()

add_subdirectory(core)
add_subdirectory(io)
add_subdirectory(ui)

qt_add_executable(circuit_capacity_calculator
    main.cpp
)

target_link_libraries(circuit_capacity_calculator PRIVATE
    ccc_core
    ccc_io
    ccc_ui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(circuit_capacity_calculator PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(circuit_capacity_calculator PRIVATE CCC_HAS_OPENMP=1)
endif()

set_target_properties(circuit_capacity_calculator PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# --- Auto-deploy Qt runtime alongside the binary on Windows / macOS --------
# Lets you double-click circuit_capacity_calculator.exe (or the .app bundle)
# without needing to set PATH. Linux uses normal RPATH mechanics.
if(WIN32)
    set(CCC_FASTCAP_EXE "${CMAKE_SOURCE_DIR}/external/fastcap/fastcap.exe")
    set(CCC_FASTERCAP_EXE "${CMAKE_SOURCE_DIR}/external/fastercap/FasterCap.exe")
    set(CCC_FASTERCAP_GOMP "${CMAKE_SOURCE_DIR}/external/fastercap/libgomp_64-1.dll")
    if(EXISTS "${CCC_FASTCAP_EXE}")
        add_custom_command(TARGET circuit_capacity_calculator POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CCC_FASTCAP_EXE}"
                    "$<TARGET_FILE_DIR:circuit_capacity_calculator>/fastcap.exe"
            COMMENT "Copying FastCap executable alongside the application"
        )
    endif()
    if(EXISTS "${CCC_FASTERCAP_EXE}")
        add_custom_command(TARGET circuit_capacity_calculator POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CCC_FASTERCAP_EXE}"
                    "$<TARGET_FILE_DIR:circuit_capacity_calculator>/FasterCap.exe"
            COMMENT "Copying FasterCap executable alongside the application"
        )
    endif()
    if(EXISTS "${CCC_FASTERCAP_GOMP}")
        add_custom_command(TARGET circuit_capacity_calculator POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CCC_FASTERCAP_GOMP}"
                    "$<TARGET_FILE_DIR:circuit_capacity_calculator>/libgomp_64-1.dll"
            COMMENT "Copying FasterCap OpenMP runtime alongside the application"
        )
    endif()

    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin}")
    if(WINDEPLOYQT_EXE)
        add_custom_command(TARGET circuit_capacity_calculator POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}" --no-translations --no-compiler-runtime
                    --no-system-d3d-compiler --no-opengl-sw
                    "$<TARGET_FILE:circuit_capacity_calculator>"
            COMMENT "windeployqt: copying Qt DLLs alongside the executable"
        )
    endif()
elseif(APPLE)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(MACDEPLOYQT_EXE macdeployqt HINTS "${_qt_bin}")
    if(MACDEPLOYQT_EXE)
        add_custom_command(TARGET circuit_capacity_calculator POST_BUILD
            COMMAND "${MACDEPLOYQT_EXE}" "$<TARGET_BUNDLE_DIR:circuit_capacity_calculator>"
            COMMENT "macdeployqt: bundling Qt frameworks into the .app"
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS circuit_capacity_calculator
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE  DESTINATION .
)
