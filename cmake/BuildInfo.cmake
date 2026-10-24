# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# Source archives can supply the full revision explicitly. Git checkouts follow
# HEAD, including linked worktrees and packed refs, on incremental builds.
set(LUXIR_SOURCE_COMMIT "" CACHE STRING "Full source commit hash (auto-detected from Git when empty)")
set(luxir_source_commit "${LUXIR_SOURCE_COMMIT}")
if(NOT luxir_source_commit)
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE luxir_source_commit OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_result)
    if(NOT git_result EQUAL 0)
      message(FATAL_ERROR "Cannot determine the Luxir source commit")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE git_ref OUTPUT_STRIP_TRAILING_WHITESPACE)
    foreach(ref HEAD packed-refs ${git_ref})
      execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${ref}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ref_path OUTPUT_STRIP_TRAILING_WHITESPACE)
      get_filename_component(ref_path "${ref_path}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      # A commit on a packed branch creates a new loose ref without changing
      # HEAD or packed-refs. Track appearance/removal as well as file contents.
      file(GLOB ref_files CONFIGURE_DEPENDS LIST_DIRECTORIES false "${ref_path}")
      if(ref_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ref_files})
      endif()
    endforeach()
  else()
    set(luxir_source_commit "unknown")
    message(WARNING "Source commit unavailable; set LUXIR_SOURCE_COMMIT for source archive builds")
  endif()
endif()
if(NOT luxir_source_commit STREQUAL "unknown")
  string(LENGTH "${luxir_source_commit}" commit_length)
  if(NOT luxir_source_commit MATCHES "^[0-9a-fA-F]+$" OR
     NOT (commit_length EQUAL 40 OR commit_length EQUAL 64))
    message(FATAL_ERROR "LUXIR_SOURCE_COMMIT must be a full Git commit hash")
  endif()
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/BuildInfo.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.cpp" @ONLY)
