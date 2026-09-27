# Locate slangc once, at configure time.
# VULKAN_SDK first: the SDK's slangc is version-matched to its validation
# layers. A stray slangc on PATH may be older than the SDK.
find_program(SLANGC_EXECUTABLE
    NAMES slangc
    PATHS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    NO_DEFAULT_PATH)

# Second pass: allow PATH if the SDK layout differs.
find_program(SLANGC_EXECUTABLE NAMES slangc)

if(NOT SLANGC_EXECUTABLE)
    # No glslc fallback on purpose: a silent fallback means compiling a
    # different language than the source is written in.
    message(FATAL_ERROR
        "slangc not found. Install the Vulkan SDK (which ships it) and ensure "
        "VULKAN_SDK is set, or put slangc on PATH.")
endif()

message(STATUS "slangc: ${SLANGC_EXECUTABLE}")

# Recursively collects the files a Slang source #include"s
function(_engine_scan_shader_includes source_file out_var)
    set(visited "")
    set(pending "${source_file}")
    set(result "")

    while(pending)
        list(GET pending 0 current)
        list(REMOVE_AT pending 0)

        if(current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")

        if(NOT EXISTS "${current}")
            continue()
        endif()

        get_filename_component(current_dir "${current}" DIRECTORY)

        file(STRINGS "${current}" include_lines
            REGEX "^[ \t]*#include[ \t]+\"[^\"]+\"")

        foreach(line IN LISTS include_lines)
            string(REGEX REPLACE "^[ \t]*#include[ \t]+\"([^\"]+)\".*" "\\1"
                included "${line}")
            get_filename_component(included_abs "${current_dir}/${included}" ABSOLUTE)
            list(APPEND result "${included_abs}")
            list(APPEND pending "${included_abs}")
        endforeach()
    endwhile()

    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# engine_add_shaders(<target>
#     SOURCE  <file.slang>
#     ENTRIES <name>:<stage> [<name>:<stage> ...])
#
# Emits one .spv per entry point, then copies them next to the executable so
# the run directory is self-contained and the exe works when moved.
function(engine_add_shaders target)
    cmake_parse_arguments(ARG "" "SOURCE" "ENTRIES" ${ARGN})

    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "engine_add_shaders: SOURCE is required")
    endif()

    if(NOT ARG_ENTRIES)
        message(FATAL_ERROR "engine_add_shaders: ENTRIES is required")
    endif()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "engine_add_shaders: unrecognised arguments (keywords are "
            "case-sensitive): ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    get_filename_component(source_abs "${ARG_SOURCE}" ABSOLUTE)
    get_filename_component(source_name "${ARG_SOURCE}" NAME_WE)

    _engine_scan_shader_includes("${source_abs}" included_deps)

    # No generator expression in this path: add_custom_command(OUTPUT) paths
    # containing a genex cannot be matched against add_custom_target(DEPENDS),
    # so the command silently never runs.
    set(gen_dir "${CMAKE_BINARY_DIR}/shaders")

    set(outputs "")

    # RenderDoc shows Slang source instead of disassembly with -g; -O0 keeps
    # the mapping honest. Unconditional for now: the output path carries no
    # configuration, so a per-config flag would leave one config reading the
    # other's .spv.
    set(debug_flags -g -O0)

    foreach(entry IN LISTS ARG_ENTRIES)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 entry_name)
        list(GET parts 1 entry_stage)

        set(spv "${gen_dir}/${source_name}.${entry_name}.spv")

        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${SLANGC_EXECUTABLE}"
                    "${source_abs}"
                    -target spirv
                    -profile spirv_1_5
                    -entry ${entry_name}
                    -stage ${entry_stage}
                    # Without this Slang renames every entry point to "main"
                    -fvk-use-entrypoint-name
                    # Matches GLM's default
                    -matrix-layout-column-major
                    ${debug_flags}
                    -o "${spv}"
            # DEPENDS on the source (plus anything it #includes,
            # transitively) gives recompile-on-edit for free.
            DEPENDS "${source_abs}" ${included_deps}
            COMMENT "slangc ${source_name}.slang [${entry_name}/${entry_stage}]"
            VERBATIM
            COMMAND_EXPAND_LISTS)

        list(APPEND outputs "${spv}")
    endforeach()

    # A custom target to manage the shaders.
    # shaders are built before the exe links and rebuilt when edited.
    set(shader_target ${target}_shaders_${source_name})
    add_custom_target(${shader_target} DEPENDS ${outputs})
    add_dependencies(${target} ${shader_target})

    # The exe resolves shaders relative to its own directory, not the CWD.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${outputs} "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMENT "Copying ${source_name} shaders next to ${target}"
        VERBATIM
        COMMAND_EXPAND_LISTS)
endfunction()
