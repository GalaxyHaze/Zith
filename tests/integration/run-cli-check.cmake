if(NOT DEFINED PROGRAM)
    message(FATAL_ERROR "PROGRAM is required")
endif()

if(NOT DEFINED EXPECTED_EXIT)
    message(FATAL_ERROR "EXPECTED_EXIT is required")
endif()

set(_args)
if(DEFINED ARGS)
    set(_args ${ARGS})
endif()

execute_process(
    COMMAND "${PROGRAM}" ${_args}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

if(NOT _result EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
        "unexpected exit code\nexpected: ${EXPECTED_EXIT}\nactual: ${_result}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()

function(_require_contains _haystack _needle _label)
    string(FIND "${_haystack}" "${_needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR
            "missing expected ${_label}: ${_needle}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endfunction()

if(DEFINED EXPECT_STDOUT)
    _require_contains("${_stdout}" "${EXPECT_STDOUT}" "stdout fragment")
endif()
if(DEFINED EXPECT_STDOUT_1)
    _require_contains("${_stdout}" "${EXPECT_STDOUT_1}" "stdout fragment")
endif()
if(DEFINED EXPECT_STDOUT_2)
    _require_contains("${_stdout}" "${EXPECT_STDOUT_2}" "stdout fragment")
endif()
if(DEFINED EXPECT_STDERR)
    _require_contains("${_stderr}" "${EXPECT_STDERR}" "stderr fragment")
endif()
if(DEFINED EXPECT_STDERR_1)
    _require_contains("${_stderr}" "${EXPECT_STDERR_1}" "stderr fragment")
endif()
if(DEFINED EXPECT_STDERR_2)
    _require_contains("${_stderr}" "${EXPECT_STDERR_2}" "stderr fragment")
endif()
