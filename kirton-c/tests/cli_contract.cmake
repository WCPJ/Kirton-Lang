function(expect_cli expected_status expected_fragment)
    execute_process(
        COMMAND "${KIRTON_EXE}" ${ARGN}
        RESULT_VARIABLE actual_status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    set(output "${stdout}${stderr}")
    if(NOT "${actual_status}" STREQUAL "${expected_status}")
        message(FATAL_ERROR
            "CLI returned '${actual_status}', expected '${expected_status}'. Output: ${output}")
    endif()
    if(NOT "${expected_fragment}" STREQUAL "")
        string(FIND "${output}" "${expected_fragment}" fragment_position)
        if(fragment_position LESS 0)
            message(FATAL_ERROR
                "CLI output did not contain '${expected_fragment}'. Output: ${output}")
        endif()
    endif()
endfunction()

expect_cli(0 "--entry" --help)
expect_cli(64 "--entry")
expect_cli(64 "--unknown" --unknown)
expect_cli(66 "missing_script.kir" "${SOURCE_DIR}/missing_script.kir")
expect_cli(65 "invalid_syntax.kir" "${SOURCE_DIR}/tests/invalid_syntax.kir")
expect_cli(1 "division_by_zero.kir" "${SOURCE_DIR}/tests/division_by_zero.kir")
expect_cli(0 "CLI_CONTRACT_OK" -e запуск
    "${SOURCE_DIR}/tests/cli_contract.kir" -- payload_Ада second)
