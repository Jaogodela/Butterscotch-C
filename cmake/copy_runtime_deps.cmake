if(NOT DEFINED INPUT_EXE OR INPUT_EXE STREQUAL "")
  message(FATAL_ERROR "copy_runtime_deps.cmake requires -DINPUT_EXE=<path>")
endif()

if(NOT DEFINED OUTPUT_DIR OR OUTPUT_DIR STREQUAL "")
  message(FATAL_ERROR "copy_runtime_deps.cmake requires -DOUTPUT_DIR=<path>")
endif()

set(BS_RUNTIME_SEARCH_DIRS "")
foreach(BS_DIR_VAR IN ITEMS SEARCH_DIRS SEARCH_DIR_COMPILER SEARCH_DIR_SDL2 SEARCH_DIR_SDL2_IMAGE SEARCH_DIR_SDL2_MIXER)
  if(DEFINED ${BS_DIR_VAR} AND NOT "${${BS_DIR_VAR}}" STREQUAL "")
    list(APPEND BS_RUNTIME_SEARCH_DIRS "${${BS_DIR_VAR}}")
  endif()
endforeach()
list(REMOVE_DUPLICATES BS_RUNTIME_SEARCH_DIRS)

set(BS_SYSTEM_DLL_NAMES
  ADVAPI32.DLL
  COMBASE.DLL
  GDI32.DLL
  IMM32.DLL
  KERNEL32.DLL
  KERNELBASE.DLL
  MSVCP_WIN.DLL
  MSVCRT.DLL
  NTDLL.DLL
  OLE32.DLL
  OLEAUT32.DLL
  RPCRT4.DLL
  SECHOST.DLL
  SETUPAPI.DLL
  SHELL32.DLL
  USER32.DLL
  VERSION.DLL
  WIN32U.DLL
  WINMM.DLL
)

# Previous builds may have copied Windows system DLLs into OUTPUT_DIR.
# Remove them first to avoid ambiguity when resolving dependencies.
foreach(BS_SYSTEM_DLL IN LISTS BS_SYSTEM_DLL_NAMES)
  file(REMOVE "${OUTPUT_DIR}/${BS_SYSTEM_DLL}")
endforeach()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${INPUT_EXE}"
  RESOLVED_DEPENDENCIES_VAR BS_RUNTIME_RESOLVED
  UNRESOLVED_DEPENDENCIES_VAR BS_RUNTIME_UNRESOLVED
  CONFLICTING_DEPENDENCIES_PREFIX BS_RUNTIME_CONFLICTING
  DIRECTORIES ${BS_RUNTIME_SEARCH_DIRS}
  PRE_EXCLUDE_REGEXES
    "api-ms-win-.*"
    "ext-ms-.*"
)

foreach(BS_DEP IN LISTS BS_RUNTIME_RESOLVED)
  get_filename_component(BS_DEP_NAME "${BS_DEP}" NAME)
  string(TOUPPER "${BS_DEP_NAME}" BS_DEP_NAME_UPPER)
  if(BS_DEP_NAME_UPPER IN_LIST BS_SYSTEM_DLL_NAMES)
    continue()
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BS_DEP}" "${OUTPUT_DIR}"
    RESULT_VARIABLE BS_COPY_RESULT
  )
  if(NOT BS_COPY_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to copy runtime dependency: ${BS_DEP}")
  endif()
endforeach()

# For conflicting dependencies (found in multiple dirs), copy the first one
# that is NOT already in OUTPUT_DIR.
foreach(BS_DEP_NAME IN LISTS BS_RUNTIME_CONFLICTING_FILENAMES)
  string(TOUPPER "${BS_DEP_NAME}" BS_DEP_NAME_UPPER)
  if(BS_DEP_NAME_UPPER IN_LIST BS_SYSTEM_DLL_NAMES)
    continue()
  endif()
  set(BS_CHOSEN_PATH "")
  foreach(BS_PATH IN LISTS BS_RUNTIME_CONFLICTING_${BS_DEP_NAME})
    string(FIND "${BS_PATH}" "${OUTPUT_DIR}" BS_IS_OUTPUT)
    if(NOT BS_IS_OUTPUT EQUAL 0 AND BS_CHOSEN_PATH STREQUAL "")
      set(BS_CHOSEN_PATH "${BS_PATH}")
    endif()
  endforeach()
  if(NOT BS_CHOSEN_PATH STREQUAL "")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BS_CHOSEN_PATH}" "${OUTPUT_DIR}"
    )
  endif()
endforeach()

if(BS_RUNTIME_UNRESOLVED)
  list(JOIN BS_RUNTIME_UNRESOLVED ", " BS_RUNTIME_UNRESOLVED_JOINED)
  message(STATUS "Unresolved runtime dependencies for ${INPUT_EXE}: ${BS_RUNTIME_UNRESOLVED_JOINED}")
endif()
