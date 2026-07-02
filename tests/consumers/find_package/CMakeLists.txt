cmake_minimum_required(VERSION 3.21)
project(microres_find_package_consumer LANGUAGES C)

find_package(microres CONFIG REQUIRED)

add_executable(microres_find_package_consumer main.c)
target_link_libraries(microres_find_package_consumer PRIVATE microres::microres)
