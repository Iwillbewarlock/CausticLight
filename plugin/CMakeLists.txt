cmake_minimum_required(VERSION 3.21)

project(CausticLight VERSION 0.1.0 LANGUAGES CXX)

# CommonLibSSE-NG (alandtse/CommonLibVR, ng) as a git submodule:
# CommonLibSSE-NG source tree under extern/, SE + AE, no VR.
set(ENABLE_SKYRIM_SE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Pure logic (no game headers) - shared by the plugin and the test executable.
# ---------------------------------------------------------------------------
add_library(CausticMath STATIC src/CausticMath.cpp)
target_include_directories(CausticMath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(CausticMath PUBLIC cxx_std_23)

# ---------------------------------------------------------------------------
# Our own draw-pass shaders, compiled offline by fxc into bytecode headers (no
# runtime compile). A shader error fails the build.
# ---------------------------------------------------------------------------
find_program(FXC_EXE fxc.exe
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    REQUIRED)
set(CL_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/CausticPass.hlsl)
set(CL_SHADER_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${CL_SHADER_GEN})
add_custom_command(
    OUTPUT ${CL_SHADER_GEN}/CausticPassVS.h
    COMMAND ${FXC_EXE} /nologo /T vs_5_0 /E VSMain /O3 /Gis /WX /Vn g_CausticPassVS /Fh ${CL_SHADER_GEN}/CausticPassVS.h ${CL_SHADER_SRC}
    DEPENDS ${CL_SHADER_SRC}
    COMMENT "fxc CausticPass.hlsl VSMain"
    VERBATIM)
add_custom_command(
    OUTPUT ${CL_SHADER_GEN}/CausticPassPS.h
    COMMAND ${FXC_EXE} /nologo /T ps_5_0 /E PSMain /O3 /Gis /WX /Vn g_CausticPassPS /Fh ${CL_SHADER_GEN}/CausticPassPS.h ${CL_SHADER_SRC}
    DEPENDS ${CL_SHADER_SRC}
    COMMENT "fxc CausticPass.hlsl PSMain"
    VERBATIM)
add_custom_command(
    OUTPUT ${CL_SHADER_GEN}/CausticCurvPS.h
    COMMAND ${FXC_EXE} /nologo /T ps_5_0 /E CurvaturePS /O3 /Gis /WX /Vn g_CausticCurvPS /Fh ${CL_SHADER_GEN}/CausticCurvPS.h ${CL_SHADER_SRC}
    DEPENDS ${CL_SHADER_SRC}
    COMMENT "fxc CausticPass.hlsl CurvaturePS"
    VERBATIM)

# ---------------------------------------------------------------------------
# SKSE plugin
# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME} SHARED)

target_sources(${PROJECT_NAME} PRIVATE
    src/plugin.cpp
    src/Settings.cpp
    src/Zones.cpp
    src/Hooks.cpp
    src/DrawPass.cpp
    ${CL_SHADER_GEN}/CausticPassVS.h
    ${CL_SHADER_GEN}/CausticPassPS.h
    ${CL_SHADER_GEN}/CausticCurvPS.h
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CL_SHADER_GEN})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE
    CausticMath
    nlohmann_json::nlohmann_json
    d3d11.lib
)

# ---------------------------------------------------------------------------
# Offline test for the weight / speed / input math. Runs without the game.
# ---------------------------------------------------------------------------
add_executable(CausticMathTest tests/CausticMathTest.cpp)
target_link_libraries(CausticMathTest PRIVATE CausticMath)

enable_testing()
add_test(NAME CausticMathTest COMMAND CausticMathTest)

# ---------------------------------------------------------------------------
# Offline GPU check: the real pass shaders over a synthetic pond scene (WARP, no game).
# ---------------------------------------------------------------------------
add_executable(PassRender tests/PassRender.cpp ${CL_SHADER_GEN}/CausticPassVS.h ${CL_SHADER_GEN}/CausticPassPS.h ${CL_SHADER_GEN}/CausticCurvPS.h)
target_include_directories(PassRender PRIVATE ${CL_SHADER_GEN})
target_link_libraries(PassRender PRIVATE CausticMath d3d11.lib d3dcompiler.lib)
add_test(NAME PassRender COMMAND PassRender ${CMAKE_CURRENT_SOURCE_DIR}/../dist/CausticLight/textures/CausticLight/calm.dds ${CMAKE_CURRENT_BINARY_DIR})
