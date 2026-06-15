cmake_minimum_required(VERSION 3.20)
project(FH6CarTool)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(GUIInjector WIN32
    GUIInjector/main.cpp
    GUIInjector/TargetedCarUnlock.cpp
    GUIInjector/TargetedCarUnlock.h
    Injector/Injector.cpp
    Injector/Injector.h
    game/cdatabase.cpp
    game/cdatabase.h
    game/sql_inject.cpp
    game/sql_inject.h
    process/process.cpp
    process/process.h
    process/remote_thread.cpp
    process/remote_thread.h
    process/signatures.cpp
    process/signatures.h
    embedded_resources.rc
    dependencies/liquid_glass/glass/glass.cpp
    dependencies/liquid_glass/glass/glass.h
    dependencies/liquid_glass/glass/shaders.h
    dependencies/liquid_glass/glass/spring.h
    dependencies/imgui/imgui.cpp
    dependencies/imgui/imgui_draw.cpp
    dependencies/imgui/imgui_tables.cpp
    dependencies/imgui/imgui_widgets.cpp
    dependencies/imgui/backends/imgui_impl_dx11.cpp
    dependencies/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(GUIInjector PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends
    ${CMAKE_SOURCE_DIR}/dependencies/json/include
    ${CMAKE_SOURCE_DIR}/dependencies/liquid_glass
    ${CMAKE_SOURCE_DIR}/dependencies/liquid_glass/glass
    ${CMAKE_SOURCE_DIR}/Injector
)

target_compile_definitions(GUIInjector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WINDOWS)
target_link_libraries(GUIInjector d3d11 d3dcompiler dxgi dwmapi winmm psapi)
