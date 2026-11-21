# Exercise the actual lockstep startup path: duplicate preparation is rejected
# before reset(), output capture or gameplay can begin. Imported ROM stays intact.
execute_process(
    COMMAND "${NATIVE}" "${ROM}" "${OUTPUT}" 1 --native-hcount --enhanced --enhanced
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 30)
if(NOT "${status}" STREQUAL "2")
    message(FATAL_ERROR "Rejected enhanced data must exit 2 before reset, got ${status}: ${stdout} ${stderr}")
endif()
if(NOT stderr MATCHES "native enhancement data already prepared at ff000000")
    message(FATAL_ERROR "Expected named first-table rejection was absent: ${stderr}")
endif()
if(stdout MATCHES "native: frames=")
    message(FATAL_ERROR "Gameplay ran after enhanced table rejection: ${stdout}")
endif()
message(STATUS "Invalid enhanced data rejected by the real startup caller before reset")
