cmake_minimum_required(VERSION 3.21)

project(microres VERSION 1.0.1 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(CTest)

option(MRES_BUILD_TESTS "Build microres tests and consumer fixtures." ON)
option(MRES_STRICT_WARNINGS "Enable strict warnings on project-owned targets." ON)
option(MRES_WITH_JITTER "Enable jitter support in the generated public config." ON)
set(MRES_MAX_ATTEMPTS_VALUE "255" CACHE STRING "Maximum public retry attempts.")

if(NOT MRES_MAX_ATTEMPTS_VALUE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MRES_MAX_ATTEMPTS_VALUE must be numeric.")
endif()

if(MRES_MAX_ATTEMPTS_VALUE LESS 1 OR MRES_MAX_ATTEMPTS_VALUE GREATER 255)
    message(FATAL_ERROR "MRES_MAX_ATTEMPTS_VALUE must be in the inclusive range 1..255.")
endif()

set(MRES_ENABLE_JITTER_VALUE 0)
if(MRES_WITH_JITTER)
    set(MRES_ENABLE_JITTER_VALUE 1)
endif()

set(MRES_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${MRES_GENERATED_INCLUDE_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mres_config.h.in"
    "${MRES_GENERATED_INCLUDE_DIR}/mres_config.h"
    @ONLY
)

add_library(microres src/mres.c)
add_library(microres::microres ALIAS microres)

target_compile_features(microres PUBLIC c_std_99)
target_include_directories(
    microres
    PUBLIC
        "$<BUILD_INTERFACE:${MRES_GENERATED_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

if(MRES_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(microres PRIVATE /W4 /WX)
    else()
        target_compile_options(microres PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endif()

set(MRES_PUBLIC_HEADERS
    include/mres.h
    "${MRES_GENERATED_INCLUDE_DIR}/mres_config.h"
)

install(TARGETS microres EXPORT microresTargets)
install(FILES ${MRES_PUBLIC_HEADERS} DESTINATION include)

install(
    EXPORT microresTargets
    FILE microresTargets.cmake
    NAMESPACE microres::
    DESTINATION lib/cmake/microres
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/microresConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/microresConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/microresConfig.cmake"
    INSTALL_DESTINATION lib/cmake/microres
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/microresConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/microresConfigVersion.cmake"
    DESTINATION lib/cmake/microres
)

function(mres_apply_warnings target_name)
    if(MRES_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /W4 /WX)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        endif()
    endif()
endfunction()

if(MRES_BUILD_TESTS)
    add_executable(microres_test tests/test_all.c)
    target_link_libraries(microres_test PRIVATE microres::microres)
    mres_apply_warnings(microres_test)
    add_test(NAME microres_runtime COMMAND microres_test)

    add_executable(microres_harness_selftest tests/harness_selftest.c)
    mres_apply_warnings(microres_harness_selftest)
    add_test(NAME microres_harness_selftest COMMAND microres_harness_selftest)
    set_tests_properties(microres_harness_selftest PROPERTIES WILL_FAIL TRUE)

    add_executable(consumer_add_subdirectory_c tests/consumers/add_subdirectory/main.c)
    target_link_libraries(consumer_add_subdirectory_c PRIVATE microres::microres)
    mres_apply_warnings(consumer_add_subdirectory_c)

    foreach(cxx_std IN ITEMS 11 17 20)
        add_executable("consumer_cpp${cxx_std}" "tests/consumers/cpp${cxx_std}/main.cpp")
        target_link_libraries("consumer_cpp${cxx_std}" PRIVATE microres::microres)
        target_compile_features("consumer_cpp${cxx_std}" PRIVATE "cxx_std_${cxx_std}")
        mres_apply_warnings("consumer_cpp${cxx_std}")
    endforeach()

    if(NOT MSVC)
        foreach(case_name IN ITEMS
                invalid_jitter
                invalid_attempts
                conflicting_generated_config
                unsupported_abi_change)
            add_test(
                NAME "compile_fail_${case_name}"
                COMMAND
                    ${CMAKE_COMMAND}
                    -DCASE_NAME=${case_name}
                    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                    -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                    -DC_COMPILER=${CMAKE_C_COMPILER}
                    -DMRES_GENERATED_INCLUDE_DIR=${MRES_GENERATED_INCLUDE_DIR}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCompileFail.cmake
            )
        endforeach()
    endif()
endif()
