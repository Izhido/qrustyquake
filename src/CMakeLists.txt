cmake_minimum_required(VERSION 3.22)

project(qrustyquake)

set(SRC_DIR "${PROJECT_SOURCE_DIR}/.")
set(SDL_DIR "${PROJECT_SOURCE_DIR}/../../SDL2-2.32.4")

file(GLOB SRC_HEADERS "${SRC_DIR}/*.h")
file(GLOB SRC_SOURCE "${SRC_DIR}/*.c")

list(FILTER SRC_SOURCE EXCLUDE REGEX "net_bsd")
list(FILTER SRC_SOURCE EXCLUDE REGEX "sys_sdl.c")

add_executable(qrustyquake ${SRC_SOURCE})

target_compile_definitions(qrustyquake PRIVATE -DWIN32_LEAN_AND_MEAN -D_USE_WINSOCK2)

target_include_directories(qrustyquake PRIVATE "." ${SDL_DIR}/include)

target_link_directories(qrustyquake PRIVATE "." ${SDL_DIR}/lib/x64)

target_link_libraries(qrustyquake SDL2 SDL2main ws2_32)