# Helper script run by the `download-bin-db` target. Skips the download if the
# file is already present and non-empty.

if(EXISTS "${BIN_DB_PATH}")
    file(SIZE "${BIN_DB_PATH}" _existing_size)
    if(_existing_size GREATER 1000000)
        message(STATUS "BIN database already exists (${_existing_size} bytes): ${BIN_DB_PATH}")
        return()
    endif()
endif()

message(STATUS "Downloading: ${BIN_DB_URL}")
file(DOWNLOAD "${BIN_DB_URL}" "${BIN_DB_PATH}"
     SHOW_PROGRESS
     STATUS  _status
     LOG     _log)

list(GET _status 0 _code)
list(GET _status 1 _msg)
if(NOT _code EQUAL 0)
    file(REMOVE "${BIN_DB_PATH}")
    message(FATAL_ERROR "Failed to download BIN database (${_code}): ${_msg}\n${_log}")
endif()

file(SIZE "${BIN_DB_PATH}" _final_size)
message(STATUS "BIN database written to ${BIN_DB_PATH} (${_final_size} bytes)")
message(STATUS "Source: ${BIN_DB_URL}")
message(STATUS "License: CC-BY-4.0 (https://github.com/venelinkochev/bin-list-data)")
