cmake_minimum_required(VERSION 3.21)
project(microres_add_subdirectory_consumer LANGUAGES C)

add_subdirectory(../../.. microres-build)

add_executable(microres_add_subdirectory_consumer main.c)
target_link_libraries(microres_add_subdirectory_consumer PRIVATE microres::microres)
