# Run the fatal contract check in its own process and verify the diagnostic.
file(MAKE_DIRECTORY "${DIR}")
execute_process(COMMAND "${WATCH}" "${MODE}" "${DIR}" RESULT_VARIABLE result
                ERROR_VARIABLE diagnostic TIMEOUT 10)
file(REMOVE "${DIR}/wake")
if(MODE STREQUAL "self")
    if(result STREQUAL "0" OR NOT diagnostic MATCHES "directory_watch destroyed from its own callback")
        message(FATAL_ERROR "missing self-destruction diagnostic: ${result} ${diagnostic}")
    endif()
elseif(NOT result STREQUAL "0")
    message(FATAL_ERROR "ordinary watch stop failed: ${result} ${diagnostic}")
endif()
