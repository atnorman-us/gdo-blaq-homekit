cmake_minimum_required(VERSION 3.16)

# Keep dependency fixes reproducible without unpublished submodule commits.
find_program(GDO_PATCH_PYTHON NAMES python3 REQUIRED)
execute_process(COMMAND "${GDO_PATCH_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/scripts/apply-dependency-patches.py"
                RESULT_VARIABLE GDO_PATCH_RESULT)
if(NOT GDO_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to apply required GDO dependency patches")
endif()

if(DEFINED ENV{HOMEKIT_PATH})
  set(HOMEKIT_PATH $ENV{HOMEKIT_PATH})
else()
  set(HOMEKIT_PATH ${CMAKE_CURRENT_LIST_DIR}/esp-homekit-sdk/)
endif(DEFINED ENV{HOMEKIT_PATH})

set(EXTRA_COMPONENT_DIRS ${HOMEKIT_PATH}/components ${HOMEKIT_PATH}/components/homekit)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gdo-blaq-homekit)
