cmake_minimum_required(VERSION 3.16)

# Mount profile — mandatory build selection.  Pass it as a CMake cache entry
# (idf.py -D MOUNT_PROFILE=nrm_ha) or as an environment variable
# (MOUNT_PROFILE=nrm_ha idf.py build).  A missing or unknown profile is a
# configuration error; the selected profile is normalized into a cache variable
# so the main component can consume it.
if(NOT DEFINED MOUNT_PROFILE OR MOUNT_PROFILE STREQUAL "")
    set(MOUNT_PROFILE "$ENV{MOUNT_PROFILE}")
endif()
if(NOT MOUNT_PROFILE MATCHES "^(nrm_ha|bft)$")
    message(FATAL_ERROR "MOUNT_PROFILE must be 'nrm_ha' or 'bft' (got '${MOUNT_PROFILE}') — pass -D MOUNT_PROFILE=nrm_ha to idf.py")
endif()
set(MOUNT_PROFILE "${MOUNT_PROFILE}" CACHE STRING "Mount profile (nrm_ha|bft)" FORCE)
set(MOUNT_PROFILE_SRC "profiles/profile_${MOUNT_PROFILE}.c" CACHE STRING "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nrm_ha)
