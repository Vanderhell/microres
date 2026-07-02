set(source_file "${SOURCE_DIR}/tests/compile_fail/${CASE_NAME}.c")
set(object_file "${BINARY_DIR}/compile_fail/${CASE_NAME}.o")
file(MAKE_DIRECTORY "${BINARY_DIR}/compile_fail")

get_filename_component(compiler_name "${C_COMPILER}" NAME_WE)

if(compiler_name STREQUAL "cl")
    execute_process(
        COMMAND
            "${C_COMPILER}"
            /nologo
            /std:c11
            /c
            "${source_file}"
            "/Fo${object_file}"
            /I
            "${SOURCE_DIR}/include"
            /I
            "${MRES_GENERATED_INCLUDE_DIR}"
            /I
            "${SOURCE_DIR}/tests/compile_fail/include"
        RESULT_VARIABLE compile_result
        OUTPUT_VARIABLE compile_stdout
        ERROR_VARIABLE compile_stderr
    )
else()
    execute_process(
        COMMAND
            "${C_COMPILER}"
            -c
            "${source_file}"
            -o
            "${object_file}"
            -I
            "${SOURCE_DIR}/include"
            -I
            "${MRES_GENERATED_INCLUDE_DIR}"
            -I
            "${SOURCE_DIR}/tests/compile_fail/include"
            -std=c99
        RESULT_VARIABLE compile_result
        OUTPUT_VARIABLE compile_stdout
        ERROR_VARIABLE compile_stderr
    )
endif()

set(compile_output "${compile_stdout}\n${compile_stderr}")

if(compile_result EQUAL 0)
    message(FATAL_ERROR "compile-fail test ${CASE_NAME} unexpectedly compiled successfully")
endif()

if(compile_output MATCHES "No such file or directory|cannot open source file|fatal error C1083")
    message(FATAL_ERROR "compile-fail test ${CASE_NAME} failed for an unrelated missing-file reason:\n${compile_output}")
endif()

set(expected_pattern "MRES_DIAG")

if(CASE_NAME STREQUAL "invalid_jitter")
    set(expected_pattern "MRES_DIAG|MRES_ENABLE_JITTER")
elseif(CASE_NAME STREQUAL "invalid_attempts")
    set(expected_pattern "MRES_DIAG|MRES_MAX_ATTEMPTS")
elseif(CASE_NAME STREQUAL "conflicting_generated_config")
    set(expected_pattern "MRES_DIAG|MRES_CONFIG_ABI_REVISION|MRES_CONFIG_ABI")
elseif(CASE_NAME STREQUAL "unsupported_abi_change")
    set(expected_pattern "MRES_DIAG|MRES_USE_BOOL_FIELDS|ABI_CHANGE")
endif()

if(NOT compile_output MATCHES "${expected_pattern}")
    message(FATAL_ERROR
        "compile-fail test ${CASE_NAME} did not report the intended diagnostic.\n"
        "Expected pattern: ${expected_pattern}\n"
        "Output:\n${compile_output}")
endif()
