find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(ovlextract_gui
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/OvlInventory.cpp
    src/OvlInventory.hpp
    src/TexturePreview.cpp
    src/TexturePreview.hpp
    src/MeshViewer.cpp
    src/MeshViewer.hpp
)

target_link_libraries(ovlextract_gui
    PRIVATE
        ovlcore
        ovlextractors
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_features(ovlextract_gui PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovlextract_gui)

# AUTOMOC pulls Qt headers that emit -Wfloat-equal noise inside moc-generated
# code on AppleClang; downgrade just for this target without touching the lib
# warnings policy.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ovlextract_gui PRIVATE
        -Wno-float-equal
        -Wno-shadow
        -Wno-sign-conversion
        -Wno-conversion
    )
endif()

set_target_properties(ovlextract_gui PROPERTIES
    MACOSX_BUNDLE FALSE
    AUTOMOC_MACRO_NAMES "Q_OBJECT;Q_GADGET;Q_NAMESPACE"
)
