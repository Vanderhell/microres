set(source_file "${SOURCE_DIR}/tests/compile_fail/${CASE_NAME}.c")
set(binary_dir "${BINARY_DIR}/compile_fail/${CASE_NAME}")

try_compile(
    COMPILE_FAILED
    "${binary_dir}"
    "${source_file}"
    CMAKE_FLAGS
        "-DCMAKE_C_STANDARD=99"
        "-DCMAKE_C_STANDARD_REQUIRED=ON"
        "-DCMAKE_C_EXTENSIONS=OFF"
        "-DINCLUDE_DIRECTORIES=${SOURCE_DIR}/tests/compile_fail/include;${SOURCE_DIR}/include"
    OUTPUT_VARIABLE COMPILE_OUTPUT
)

if(COMPILE_FAILED)
    message(FATAL_ERROR "compile-fail test ${CASE_NAME} unexpectedly compiled successfully")
endif()

string(TOUPPER "${CASE_NAME}" CASE_TOKEN)
if(NOT COMPILE_OUTPUT MATCHES "MRES_DIAG")
    message(FATAL_ERROR "compile-fail test ${CASE_NAME} did not report the intended diagnostic")
endif()
