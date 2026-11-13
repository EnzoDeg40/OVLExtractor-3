# modkit: the modding layer on top of ovlcore. rct3mod::PathType,
# SceneryItem… (content built from a game object), mod.toml manifests,
# install / uninstall into the game's Assets folder, and the rct3mod CLI.

include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

add_library(rct3modkit STATIC
    src/Mod.cpp
    src/PathType.cpp
    src/SceneryItem.cpp
)
target_include_directories(rct3modkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rct3modkit PUBLIC ovlcore PRIVATE tomlplusplus::tomlplusplus)
target_compile_features(rct3modkit PUBLIC cxx_std_17)
ovl_set_compile_warnings(rct3modkit)

add_executable(rct3mod rct3mod.cpp)
target_link_libraries(rct3mod PRIVATE rct3modkit)
target_include_directories(rct3mod PRIVATE ${CMAKE_SOURCE_DIR}/third_party/CLI11)
ovl_set_compile_warnings(rct3mod)
