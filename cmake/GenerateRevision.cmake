# Preserve the original metadata format without requiring a POSIX shell or
# unquoted shell paths (the Windows workspace can contain spaces).
set(REVISION_HASH "????")
if(GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${REVISION_SOURCE_DIR}"
        OUTPUT_VARIABLE REVISION_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE REVISION_RESULT ERROR_QUIET)
    if(NOT REVISION_RESULT EQUAL 0 OR REVISION_HASH STREQUAL "")
        set(REVISION_HASH "????")
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" diff-index --quiet HEAD --
            WORKING_DIRECTORY "${REVISION_SOURCE_DIR}"
            RESULT_VARIABLE DIRTY_RESULT ERROR_QUIET)
        if(NOT DIRTY_RESULT EQUAL 0)
            string(APPEND REVISION_HASH "+")
        endif()
    endif()
endif()
string(TIMESTAMP REVISION_TIMESTAMP "%s" UTC)
file(WRITE "${REVISION_OUTPUT}"
    "#include \"Revision.hh\"\n\nconst char* GIT_REVISION_HASH = \"${REVISION_HASH}\";\nconst uint64_t BUILD_TIMESTAMP = static_cast<uint64_t>(${REVISION_TIMESTAMP}) * 1000000;\n")
