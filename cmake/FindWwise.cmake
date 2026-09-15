



if (TARGET Wwise::Wwise)
    return()
endif()

# resolve SDK Root
#Read from environment'
if (NOT WWISE_SDK_ROOT)
    if (DEFINED ENV{WWISESDK})
        set(_wwise_root "$ENV{WWISESDK}")
    elseif(DEFINED ENV{WWISEROOT})
        set(_wwise_root "$ENV{WWISEROOT}/SDK")
    endif()

    if(_wwise_root)
        file(TO_CMAKE_PATH "${_wwise_root}" _wwise_root)
        set(WWISE_SDK_ROOT "${_wwise_root}" CACHE PATH "Wwise SDK directory")
    endif()
endif()

if(NOT WWISE_SDK_ROOT)
    message(FATAL_ERROR "WWISE_SDK_ROOT is not set. Please set it to the Wwise SDK directory.")
endif()

if(NOT IS_DIRECTORY "${WWISE_SDK_ROOT}")
    message(FATAL_ERROR "WWISE_SDK_ROOT is not a valid directory: ${WWISE_SDK_ROOT}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Wwise SDK requires a 64-bit build.")
endif()

if(MSVC_TOOLSET_VERSION GREATER_EQUAL 144)
    set(_wwise_toolsets vc180 vc170 vc160)
elseif(MSVC_TOOLSET_VERSION GREATER_EQUAL 143)
    set(_wwise_toolsets vc170 vc180 vc160)
else()
    set(_wwise_toolsets vc160 vc170 vc180)
endif()

set(_wwise_platform "")
foreach(_toolset IN LISTS _wwise_toolsets)
    if(IS_DIRECTORY "${WWISE_SDK_ROOT}/x64_${_toolset}")
        set(_wwise_platform "x64_${_toolset}")
        break()
    endif()
endforeach()

if(NOT _wwise_platform)
    file(GLOB _available RELATIVE "${WWISE_SDK_ROOT}" "${WWISE_SDK_ROOT}/x64_*")
    message(FATAL_ERROR "No x64 prebuilt libraries in ${WWISE_SDK_ROOT}.\n"
        "Looked for: x64_${_wwise_toolsets}\n"
        "Found: ${_available}\n"
        "Install the Windows deployment platform for this Wwise version via "
        "the Launcher.")
endif()

#headers
find_path(WWISE_INCLUDE_DIR NAMES "AK/SoundEngine/Common/AkSoundEngine.h" PATHS "${WWISE_SDK_ROOT}/include" NO_DEFAULT_PATH)

if(NOT WWISE_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find Wwise include directory.")
endif()

#libraries
set(_wwise_cfg_DEBUG Debug)
set(_wwise_cfg_RELEASE Release)
set(_wwise_cfg_RELWITHDEBINFO Profile)
set(_wwise_cfg_MINSIZEREL Release)

set(_wwise_cmake_configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
set(_wwise_libs_common AkSoundEngine AkMemoryMgr AkStreamMgr)

set(_wwise_libs_profiling CommunicationCentral)

function(_wwise_add_imported_lib lib profiling_only)
    add_library(Wwise::${lib} STATIC IMPORTED)
    set_target_properties(Wwise::${lib} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WWISE_INCLUDE_DIR}")
    set(found_configs "")
    foreach(cfg IN LISTS _wwise_cmake_configs)
        set(wwise_cfg "${_wwise_cfg_${cfg}}")
        if(profiling_only AND wwise_cfg STREQUAL "Release")
            continue()
        endif()
        find_library(WWISE_${lib}_LIBRARY_${cfg}
            NAMES ${lib}
            PATHS "${WWISE_SDK_ROOT}/${_wwise_platform}/${wwise_cfg}/lib"
            NO_DEFAULT_PATH)
        if(NOT WWISE_${lib}_LIBRARY_${cfg})
            message(FATAL_ERROR "Could not find Wwise library ${lib} for configuration ${cfg}.")
        endif()
        set_property(TARGET Wwise::${lib} APPEND PROPERTY
            IMPORTED_CONFIGURATIONS ${cfg})
        set_target_properties(Wwise::${lib} PROPERTIES
            IMPORTED_LOCATION_${cfg} "${WWISE_${lib}_LIBRARY_${cfg}}")
        list(APPEND found_configs ${cfg})
    endforeach()
    list(GET found_configs 0 fallback_cfg)
    set_target_properties(Wwise::${lib} PROPERTIES
        IMPORTED_LOCATION "${WWISE_${lib}_LIBRARY_${fallback_cfg}}")
endfunction()

foreach(_lib IN LISTS _wwise_libs_common)
    _wwise_add_imported_lib(${_lib} FALSE)
endforeach()

foreach(_lib IN LISTS _wwise_libs_profiling)
    _wwise_add_imported_lib(${_lib} TRUE)
endforeach()

set(_wwise_link_libs ${_wwise_libs_common})
list(TRANSFORM _wwise_link_libs PREPEND Wwise::)

foreach(_lib IN LISTS _wwise_libs_profiling)
    list(APPEND _wwise_link_libs "$<$<NOT:$<CONFIG:Release,MinSizeRel>>:Wwise::${_lib}>")
endforeach()

list(APPEND _wwise_link_libs "$<$<NOT:$<CONFIG:Release,MinSizeRel>>:ws2_32>")

list(APPEND _wwise_link_libs ole32 user32 advapi32)

add_library(Wwise::Wwise INTERFACE IMPORTED)

set_target_properties(Wwise::Wwise PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${WWISE_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${_wwise_link_libs}"
    INTERFACE_COMPILE_DEFINITIONS "$<$<CONFIG:Release,MinSizeRel>:AK_OPTIMIZED>")


set(WWISE_FOUND TRUE)
message(STATUS "Wwise SDK: ${WWISE_SDK_ROOT} (${_wwise_platform})")
