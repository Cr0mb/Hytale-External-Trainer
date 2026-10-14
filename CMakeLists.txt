cmake_minimum_required(VERSION 3.20)
project(HytaleTrainer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_dx11.cpp
    third_party/imgui/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX IMGUI_DISABLE_DEMO_WINDOWS)

add_executable(HytaleTrainer WIN32
    src/main.cpp src/game.cpp src/camera.cpp src/cent.cpp src/ingame.cpp src/attach.cpp src/log.cpp src/app.rc)
target_include_directories(HytaleTrainer PRIVATE src)
target_compile_options(HytaleTrainer PRIVATE /W3 /permissive- /utf-8)
target_compile_definitions(HytaleTrainer PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(HytaleTrainer PRIVATE imgui d3d11 dxgi dcomp dwmapi ws2_32 shell32 winmm)

# In-game ESP renderer, loaded into HytaleClient.exe (see hook/render.cpp).
# Its own copy of Dear ImGui, with the OpenGL3 backend the game needs.
add_library(hytrainer-render SHARED
    hook/render.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_opengl3.cpp)
target_include_directories(hytrainer-render PRIVATE third_party/imgui src)
target_compile_options(hytrainer-render PRIVATE /W3 /utf-8)
target_compile_definitions(hytrainer-render PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DISABLE_DEMO_WINDOWS)
target_link_libraries(hytrainer-render PRIVATE opengl32)
add_dependencies(HytaleTrainer hytrainer-render)

# Single-file build: the agent jars (built by agent/build_agent.cmd before CMake
# runs) and the in-game renderer DLL are linked into HytaleTrainer.exe as RCDATA
# resources 101..103 (src/embedded.h unpacks them at run time).
set(HYT_AGENT_DIR "${CMAKE_BINARY_DIR}/Release/agent")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc" CONTENT
"101 RCDATA \"${HYT_AGENT_DIR}/hytrainer-agent.jar\"
102 RCDATA \"${HYT_AGENT_DIR}/hytrainer-core.jar\"
103 RCDATA \"$<TARGET_FILE:hytrainer-render>\"
")
target_sources(HytaleTrainer PRIVATE "${CMAKE_BINARY_DIR}/embed_$<CONFIG>.rc")
