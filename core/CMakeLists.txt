add_library(ovlcore STATIC
    src/BinaryReader.cpp
    src/OvlParser.cpp
    src/OvlImage.cpp
    src/OvlBuilder.cpp
    src/TextOvl.cpp
    src/PairView.cpp
    src/SceneryCodec.cpp
    src/PairClone.cpp
    src/ImageCodec.cpp
    src/ImageFile.cpp
)

target_include_directories(ovlcore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ovlcore PUBLIC cxx_std_17)
ovl_set_compile_warnings(ovlcore)

set_target_properties(ovlcore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# stb_image (PNG / JPEG / BMP input for the writers), header only, pinned.
include(FetchContent)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        9600522de53973336fa033a289858a4354cbb04c
)
FetchContent_MakeAvailable(stb)
target_include_directories(ovlcore SYSTEM PRIVATE ${stb_SOURCE_DIR})
