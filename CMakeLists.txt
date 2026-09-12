cmake_minimum_required(VERSION 3.10)
project(Mist_AI_Turbo_CPP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 寻找 OpenCV
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV library status:")
message(STATUS "    version: ${OpenCV_VERSION}")
message(STATUS "    libraries: ${OpenCV_LIBS}")
message(STATUS "    include path: ${OpenCV_INCLUDE_DIRS}")

# 设置头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

# 设置库文件目录
link_directories(
    ${CMAKE_SOURCE_DIR}/lib
)

# 编译可执行文件
set(AIMBOT_SOURCES
    src/main.cpp
    src/aimbot_camera.cpp
    src/aimbot_config.cpp
    src/aimbot_crosshair.cpp
    src/aimbot_labels.cpp
    src/aimbot_pid.cpp
    src/aimbot_prediction.cpp
    src/aimbot_rknn.cpp
)

add_executable(aimbot_main ${AIMBOT_SOURCES})

# 链接库 (OpenCV, RKNN 运行时, 多线程库 pthread)
target_link_libraries(aimbot_main 
    ${OpenCV_LIBS} 
    rknnrt 
    pthread
)

add_executable(hdmirx_jpeg_udp
    src/hdmirx_jpeg_udp.cpp
)

target_link_libraries(hdmirx_jpeg_udp
    ${OpenCV_LIBS}
    pthread
)

add_executable(hdmirx_v4l2_jpeg_udp
    src/hdmirx_v4l2_jpeg_udp.cpp
)

target_link_libraries(hdmirx_v4l2_jpeg_udp
    ${OpenCV_LIBS}
    pthread
)
