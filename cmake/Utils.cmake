################################################################################################
# Exclude and prepend functionalities
function (exclude OUTPUT INPUT)
set(EXCLUDES ${ARGN})
foreach(EXCLUDE ${EXCLUDES})
        list(REMOVE_ITEM INPUT "${EXCLUDE}")
endforeach()
set(${OUTPUT} ${INPUT} PARENT_SCOPE)
endfunction(exclude)

function (prepend OUTPUT PREPEND)
set(OUT "")
foreach(ITEM ${ARGN})
        list(APPEND OUT "${PREPEND}${ITEM}")
endforeach()
set(${OUTPUT} ${OUT} PARENT_SCOPE)
endfunction(prepend)


################################################################################################
# Clears variables from list
# Usage:
#   caffe_clear_vars(<variables_list>)
macro(caffe_clear_vars)
  foreach(_var ${ARGN})
    unset(${_var})
  endforeach()
endmacro()

################################################################################################
# Prints list element per line
# Usage:
#   caffe_print_list(<list>)
function(caffe_print_list)
  foreach(e ${ARGN})
    message(STATUS ${e})
  endforeach()
endfunction()

################################################################################################
# Reads set of version defines from the header file
# Usage:
#   caffe_parse_header(<file> <define1> <define2> <define3> ..)
macro(caffe_parse_header FILENAME FILE_VAR)
  set(vars_regex "")
  set(__parnet_scope OFF)
  set(__add_cache OFF)
  foreach(name ${ARGN})
    if("${name}" STREQUAL "PARENT_SCOPE")
      set(__parnet_scope ON)
    elseif("${name}" STREQUAL "CACHE")
      set(__add_cache ON)
    elseif(vars_regex)
      set(vars_regex "${vars_regex}|${name}")
    else()
      set(vars_regex "${name}")
    endif()
  endforeach()
  if(EXISTS "${FILENAME}")
    file(STRINGS "${FILENAME}" ${FILE_VAR} REGEX "#define[ \t]+(${vars_regex})[ \t]+[0-9]+" )
  else()
    unset(${FILE_VAR})
  endif()
  foreach(name ${ARGN})
    if(NOT "${name}" STREQUAL "PARENT_SCOPE" AND NOT "${name}" STREQUAL "CACHE")
      if(${FILE_VAR})
        if(${FILE_VAR} MATCHES ".+[ \t]${name}[ \t]+([0-9]+).*")
          string(REGEX REPLACE ".+[ \t]${name}[ \t]+([0-9]+).*" "\\1" ${name} "${${FILE_VAR}}")
        else()
          set(${name} "")
        endif()
        if(__add_cache)
          set(${name} ${${name}} CACHE INTERNAL "${name} parsed from ${FILENAME}" FORCE)
        elseif(__parnet_scope)
          set(${name} "${${name}}" PARENT_SCOPE)
        endif()
      else()
        unset(${name} CACHE)
      endif()
    endif()
  endforeach()
endmacro()

################################################################################################
# Reads single version define from the header file and parses it
# Usage:
#   caffe_parse_header_single_define(<library_name> <file> <define_name>)
function(caffe_parse_header_single_define LIBNAME HDR_PATH VARNAME)
  set(${LIBNAME}_H "")
  if(EXISTS "${HDR_PATH}")
    file(STRINGS "${HDR_PATH}" ${LIBNAME}_H REGEX "^#define[ \t]+${VARNAME}[ \t]+\"[^\"]*\".*$" LIMIT_COUNT 1)
  endif()

  if(${LIBNAME}_H)
    string(REGEX REPLACE "^.*[ \t]${VARNAME}[ \t]+\"([0-9]+).*$" "\\1" ${LIBNAME}_VERSION_MAJOR "${${LIBNAME}_H}")
    string(REGEX REPLACE "^.*[ \t]${VARNAME}[ \t]+\"[0-9]+\\.([0-9]+).*$" "\\1" ${LIBNAME}_VERSION_MINOR  "${${LIBNAME}_H}")
    string(REGEX REPLACE "^.*[ \t]${VARNAME}[ \t]+\"[0-9]+\\.[0-9]+\\.([0-9]+).*$" "\\1" ${LIBNAME}_VERSION_PATCH "${${LIBNAME}_H}")
    set(${LIBNAME}_VERSION_MAJOR ${${LIBNAME}_VERSION_MAJOR} ${ARGN} PARENT_SCOPE)
    set(${LIBNAME}_VERSION_MINOR ${${LIBNAME}_VERSION_MINOR} ${ARGN} PARENT_SCOPE)
    set(${LIBNAME}_VERSION_PATCH ${${LIBNAME}_VERSION_PATCH} ${ARGN} PARENT_SCOPE)
    set(${LIBNAME}_VERSION_STRING "${${LIBNAME}_VERSION_MAJOR}.${${LIBNAME}_VERSION_MINOR}.${${LIBNAME}_VERSION_PATCH}" PARENT_SCOPE)

    # append a TWEAK version if it exists:
    set(${LIBNAME}_VERSION_TWEAK "")
    if("${${LIBNAME}_H}" MATCHES "^.*[ \t]${VARNAME}[ \t]+\"[0-9]+\\.[0-9]+\\.[0-9]+\\.([0-9]+).*$")
      set(${LIBNAME}_VERSION_TWEAK "${CMAKE_MATCH_1}" ${ARGN} PARENT_SCOPE)
    endif()
    if(${LIBNAME}_VERSION_TWEAK)
      set(${LIBNAME}_VERSION_STRING "${${LIBNAME}_VERSION_STRING}.${${LIBNAME}_VERSION_TWEAK}" ${ARGN} PARENT_SCOPE)
    else()
      set(${LIBNAME}_VERSION_STRING "${${LIBNAME}_VERSION_STRING}" ${ARGN} PARENT_SCOPE)
    endif()
  endif()
endfunction()

##############################################################################
# Helper function to add as-needed flag around a library.
function(caffe_add_as_needed_flag lib output_var)
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    # TODO: Clang seems to not need this flag. Double check.
    set(${output_var} ${lib} PARENT_SCOPE)
  elseif(MSVC)
    # TODO: check what is the behavior of MSVC.
    # In MSVC, we will add whole archive in default.
    set(${output_var} ${lib} PARENT_SCOPE)
  else()
    # Assume everything else is like gcc: we will need as-needed flag.
    set(${output_var} -Wl,--no-as-needed ${lib} -Wl,--as-needed PARENT_SCOPE)
  endif()
endfunction()

##############################################################################
# Helper function to add whole_archive flag around a library.
function(caffe_add_whole_archive_flag lib output_var)
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(${output_var} -Wl,-force_load,$<TARGET_FILE:${lib}> PARENT_SCOPE)
  elseif(MSVC)
    # In MSVC, we will add whole archive in default.
    set(${output_var} -WHOLEARCHIVE:$<TARGET_FILE:${lib}> PARENT_SCOPE)
  else()
    # Assume everything else is like gcc
    set(${output_var} -Wl,--whole-archive $<TARGET_FILE:${lib}> -Wl,--no-whole-archive PARENT_SCOPE)
  endif()
endfunction()

##############################################################################
# Helper function to add either as-needed, or whole_archive flag around a library.
function(caffe_add_linker_flag lib output_var)
  if (BUILD_SHARED_LIBS)
    caffe_add_as_needed_flag(${lib} tmp)
  else()
    caffe_add_whole_archive_flag(${lib} tmp)
  endif()
  set(${output_var} ${tmp} PARENT_SCOPE)
endfunction()

##############################################################################
# Helper function to automatically generate __init__.py files where python
# sources reside but there are no __init__.py present.
function(caffe_autogen_init_py_files)
  file(GLOB_RECURSE all_python_files RELATIVE ${PROJECT_SOURCE_DIR}
       "${PROJECT_SOURCE_DIR}/caffe2/*.py")
  set(python_paths_need_init_py)
  foreach(python_file ${all_python_files})
    get_filename_component(python_path ${python_file} PATH)
    string(REPLACE "/" ";" path_parts ${python_path})
    set(rebuilt_path ${CMAKE_BINARY_DIR})
    foreach(path_part ${path_parts})
      set(rebuilt_path "${rebuilt_path}/${path_part}")
      list(APPEND python_paths_need_init_py ${rebuilt_path})
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES python_paths_need_init_py)
  # Since the _pb2.py files are yet to be created, we will need to manually
  # add them to the list.
  list(APPEND python_paths_need_init_py ${CMAKE_BINARY_DIR}/caffe)
  list(APPEND python_paths_need_init_py ${CMAKE_BINARY_DIR}/caffe/proto)
  list(APPEND python_paths_need_init_py ${CMAKE_BINARY_DIR}/caffe2/proto)

  foreach(tmp ${python_paths_need_init_py})
    if(NOT EXISTS ${tmp}/__init__.py)
      # message(STATUS "Generate " ${tmp}/__init__.py)
      file(WRITE ${tmp}/__init__.py "")
    endif()
  endforeach()
endfunction()

##############################################################################
# Creating a Caffe2 binary target with sources specified with relative path.
# Usage:
#   caffe2_binary_target(target_name_or_src <src1> [<src2>] [<src3>] ...)
# If only target_name_or_src is specified, this target is build with one single
# source file and the target name is autogen from the filename. Otherwise, the
# target name is given by the first argument and the rest are the source files
# to build the target.
function(caffe2_binary_target target_name_or_src)
  if (${ARGN})
    set(__target ${target_name_or_src})
    prepend(__srcs "${CMAKE_CURRENT_SOURCE_DIR}/" "${ARGN}")
  else()
    get_filename_component(__target ${target_name_or_src} NAME_WE)
    prepend(__srcs "${CMAKE_CURRENT_SOURCE_DIR}/" "${target_name_or_src}")
  endif()
  add_executable(${__target} ${__srcs})
  add_dependencies(${__target} ${Caffe2_MAIN_LIBS_ORDER})
  if (USE_CUDA)
    target_link_libraries(${__target} ${Caffe2_MAIN_LIBS} ${Caffe2_DEPENDENCY_LIBS} ${Caffe2_CUDA_DEPENDENCY_LIBS})
  else()
    target_link_libraries(${__target} ${Caffe2_MAIN_LIBS} ${Caffe2_DEPENDENCY_LIBS})
  endif()
  install(TARGETS ${__target} DESTINATION bin)
endfunction()

##############################################################################
# Helper function to add paths to system include directories.
#
# Anaconda distributions typically contain a lot of packages and some
# of those can conflict with headers/libraries that must be sourced
# from elsewhere. This helper ensures that Anaconda paths are always
# added BEFORE other include paths. This prevents a common case where
# libraries and binaries are linked from Anaconda but headers are
# included from system packages, since system include directories come
# before Anaconda include directories by default. 
#
# This is just a heuristic and does not have any guarantees. We can
# add other corner cases here (as long as they are generic enough).
# A complete include path cross checker is a final resort if this
# hacky approach proves insufficient.
#
function(caffe2_include_directories)
  foreach(path IN LISTS ARGN)
    if (${DEPRIORITIZE_ANACONDA})
      # When not preferring anaconda, always search system header files before
      # anaconda include directories
      if (${path} MATCHES "/anaconda")
        include_directories(AFTER ${path})
      else()
        include_directories(BEFORE ${path})
      endif()
    else()
      # When prefering Anaconda, always search anaconda for header files before
      # system include directories
      if (${path} MATCHES "/anaconda")
        include_directories(BEFORE ${path})
      else()
        include_directories(AFTER ${path})
      endif()
    endif()
  endforeach()
endfunction()


###
# Removes common indentation from a block of text to produce code suitable for
# setting to `python -c`, or using with pycmd. This allows multiline code to be
# nested nicely in the surrounding code structure.
#
# This function respsects PYTHON_EXECUTABLE if it defined, otherwise it uses
# `python` and hopes for the best. An error will be thrown if it is not found.
#
# Args:
#     outvar : variable that will hold the stdout of the python command
#     text   : text to remove indentation from
#
function(dedent outvar text)
  # Use PYTHON_EXECUTABLE if it is defined, otherwise default to python
  if ("${PYTHON_EXECUTABLE}" STREQUAL "")
    set(_python_exe "python")
  else()
    set(_python_exe "${PYTHON_EXECUTABLE}")
  endif()
  set(_fixup_cmd "import sys; from textwrap import dedent; print(dedent(sys.stdin.read()))")
  # Use echo to pipe the text to python's stdinput. This prevents us from
  # needing to worry about any sort of special escaping.
  execute_process(
    COMMAND echo "${text}"
    COMMAND "${_python_exe}" -c "${_fixup_cmd}"
    RESULT_VARIABLE _dedent_exitcode
    OUTPUT_VARIABLE _dedent_text)
  if(NOT ${_dedent_exitcode} EQUAL 0)
    message(ERROR " Failed to remove indentation from: \n\"\"\"\n${text}\n\"\"\"
    Python dedent failed with error code: ${_dedent_exitcode}")
    message(FATAL_ERROR " Python dedent failed with error code: ${_dedent_exitcode}")
  endif()
  # Remove supurflous newlines (artifacts of print)
  string(STRIP "${_dedent_text}" _dedent_text)
  set(${outvar} "${_dedent_text}" PARENT_SCOPE)
endfunction()


###
# Helper function to run `python -c "<cmd>"` and capture the results of stdout
#
# Runs a python command and populates an outvar with the result of stdout.
# Common indentation in the text of `cmd` is removed before the command is
# executed, so the caller does not need to worry about indentation issues.
#
# This function respsects PYTHON_EXECUTABLE if it defined, otherwise it uses
# `python` and hopes for the best. An error will be thrown if it is not found.
#
# Args:
#     outvar : variable that will hold the stdout of the python command
#     cmd    : text representing a (possibly multiline) block of python code
#
function(pycmd outvar cmd)
  dedent(_dedent_cmd "${cmd}")
  # Use PYTHON_EXECUTABLE if it is defined, otherwise default to python
  if ("${PYTHON_EXECUTABLE}" STREQUAL "")
    set(_python_exe "python")
  else()
    set(_python_exe "${PYTHON_EXECUTABLE}")
  endif()
  # run the actual command
  execute_process(
    COMMAND "${_python_exe}" -c "${_dedent_cmd}"
    RESULT_VARIABLE _exitcode
    OUTPUT_VARIABLE _output)
  if(NOT ${_exitcode} EQUAL 0)
    message(ERROR " Failed when running python code: \"\"\"\n${_dedent_cmd}\n\"\"\"")
    message(FATAL_ERROR " Python command failed with error code: ${_exitcode}")
  endif()
  # Remove supurflous newlines (artifacts of print)
  string(STRIP "${_output}" _output)
  set(${outvar} "${_output}" PARENT_SCOPE)
endfunction()
