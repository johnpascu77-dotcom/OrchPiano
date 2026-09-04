# Writes a tiny header defining MACRO_NAME as the current local timestamp.
# Invoked via add_custom_target(... ALL ...), which (unlike add_custom_command
# with an OUTPUT) has no file-based up-to-date check, so this runs on every
# single build regardless of which other source files changed - fixing the
# problem where __DATE__/__TIME__ baked into one rarely-touched .cpp goes
# stale the moment an incremental build only recompiles a DIFFERENT file.

if (NOT DEFINED HEADER_PATH OR NOT DEFINED MACRO_NAME)
    message(FATAL_ERROR "HEADER_PATH and MACRO_NAME must be defined")
endif()

string(TIMESTAMP NOW "%Y-%m-%d %H:%M:%S")

get_filename_component(HEADER_DIR "${HEADER_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${HEADER_DIR}")

file(WRITE "${HEADER_PATH}" "#pragma once\n#define ${MACRO_NAME} \"${NOW}\"\n")
