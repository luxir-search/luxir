# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# Source archives can supply the full revision explicitly. Git checkouts follow
# HEAD on incremental builds: CMake re-runs when the ref storage HEAD resolves
# through changes, whether loose, packed, or reftable, in the main checkout or a
# linked worktree. Retargeting a symbolic ref between HEAD and its branch is not
# tracked.
set(LUXIR_SOURCE_COMMIT "" CACHE STRING "Full source commit hash (auto-detected from Git when empty)")
set(luxir_source_commit "${LUXIR_SOURCE_COMMIT}")
if(NOT luxir_source_commit)
  set(luxir_source_commit "unknown")
  find_package(Git QUIET)
  # Only this checkout's own repository: an archive unpacked inside an
  # unrelated repository must not report that repository's commit.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    set(git_error "not a Git checkout")
  elseif(NOT GIT_FOUND)
    set(git_error "git not found")
  else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE git_result
      OUTPUT_VARIABLE git_head OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_VARIABLE git_error ERROR_STRIP_TRAILING_WHITESPACE)
  endif()
  if(git_result EQUAL 0)
    set(luxir_source_commit "${git_head}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE git_ref OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(git_paths --git-path HEAD --git-path packed-refs)
    if(git_ref)
      list(APPEND git_paths --git-path "${git_ref}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute
        --git-dir --git-common-dir ${git_paths}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE git_paths OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\n" ";" git_paths "${git_paths}")
    list(POP_FRONT git_paths git_dir git_common_dir)
    # Reftable keeps a worktree's HEAD in its own stack beside the shared one;
    # moving the branch from another worktree changes only the shared list.
    list(APPEND git_paths "${git_dir}/reftable/tables.list" "${git_common_dir}/reftable/tables.list")
    list(REMOVE_DUPLICATES git_paths)
    foreach(path IN LISTS git_paths)
      # A commit on a packed branch creates a new loose ref without changing
      # HEAD or packed-refs. Track appearance/removal as well as file contents.
      file(GLOB tracked CONFIGURE_DEPENDS LIST_DIRECTORIES false "${path}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${tracked})
    endforeach()
  else()
    message(WARNING "Source commit unknown (${git_error}); "
                    "set LUXIR_SOURCE_COMMIT for source archive builds")
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
