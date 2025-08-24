cmake_minimum_required(VERSION 3.10)
project(transcode)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib)
add_executable(transcode src/transcode.cpp)
target_link_libraries(transcode
    glm
    GLEW
    glfw
    avformat
    avcodec
    avutil
    swscale
    swresample
    freetype
)

