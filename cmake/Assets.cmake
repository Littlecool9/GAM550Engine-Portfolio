# engine_cook_assets(<target>
#     SOURCE_ROOTS <dir> [<dir> ...]
#     OUT_DIR <dir>)
#
# SOURCE_ROOTS are relative to the repository root, not to the calling
# CMakeLists. A subdirectory therefore passes "games/foo/assets", not "assets":
# the root string is hashed into every asset id, so it has to mean the same
# thing no matter which CMakeLists asks for it.
#
# Runs AssetCooker over every SOURCE_ROOTS directory into OUT_DIR, then copies
# OUT_DIR next to the executable as cooked/ so the run directory is
# self-contained and the exe works when moved.
function(engine_cook_assets target)
    cmake_parse_arguments(ARG "" "OUT_DIR" "SOURCE_ROOTS" ${ARGN})

    if(NOT ARG_SOURCE_ROOTS)
        message(FATAL_ERROR "engine_cook_assets: SOURCE_ROOTS is required")
    endif()

    if(NOT ARG_OUT_DIR)
        message(FATAL_ERROR "engine_cook_assets: OUT_DIR is required")
    endif()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "engine_cook_assets: unrecognised arguments (keywords are "
            "case-sensitive): ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(source_files "")
    set(cooker_args "")

    foreach(root IN LISTS ARG_SOURCE_ROOTS)
        if(IS_ABSOLUTE "${root}")
            message(FATAL_ERROR
                "engine_cook_assets: SOURCE_ROOTS must be relative to the repository "
                "root, but got the absolute path \"${root}\". The root is hashed into "
                "every asset id, so an absolute path would bake this machine's "
                "checkout location into ids that must match on every machine.")
        endif()

        # BASE_DIR is the repository root, not CMAKE_CURRENT_SOURCE_DIR: the
        # cooker below resolves the same string against CMAKE_SOURCE_DIR (its
        # WORKING_DIRECTORY), and the two must agree or the glob watches one
        # directory while the cooker reads another.
        get_filename_component(root_abs "${root}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

        # CONFIGURE_DEPENDS: a newly added asset file needs a reconfigure to
        # join this list, same reasoning as vcpkg.json in the root CMakeLists.
        file(GLOB_RECURSE root_files CONFIGURE_DEPENDS "${root_abs}/*")
        # AssetCooker skips terrain recipes and release files (IsTerrainFileExcludedFromCook);
        # TerrainTool sync handles them, so editing a recipe must not rerun the cook.
        list(FILTER root_files EXCLUDE REGEX "(\\.terrain\\.json|\\.tindex|\\.tpack)$")
        list(APPEND source_files ${root_files})

        # Passed as given, not resolved to absolute: AssetCooker hashes
        # "<root>/<relative path>" into each asset's id, so an absolute path
        # here would bake this machine's checkout location into every id.
        list(APPEND cooker_args --source "${root}")
    endforeach()

    set(out_dir "${ARG_OUT_DIR}")

    # AssetCooker decides output filenames itself (hashed asset ids), so
    # unlike Shaders.cmake the exact OUTPUT set isn't known at configure time.
    # A stamp file stands in for it; DEPENDS on every source file still gives
    # recook-on-edit, and DEPENDS on the AssetCooker target recooks when the
    # cooker itself changes.
    set(stamp "${CMAKE_BINARY_DIR}/${target}_assets_cooked.stamp")
    # A removed file has no mtime left to invalidate the stamp. Track membership
    # as well as individual file contents (also covers deleted glTF subassets).
    set(source_list "${CMAKE_BINARY_DIR}/${target}_asset_sources.txt")
    list(SORT source_files)
    file(CONFIGURE OUTPUT "${source_list}" CONTENT "${source_files}\n" @ONLY)

    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND AssetCooker ${cooker_args} --out "${out_dir}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS AssetCooker "${source_list}" ${source_files}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Cooking assets for ${target}"
        VERBATIM
        COMMAND_EXPAND_LISTS)

    set(cook_target ${target}_cook_assets)
    add_custom_target(${cook_target} DEPENDS "${stamp}")
    add_dependencies(${target} ${cook_target})

    set(copy_stamp "${CMAKE_BINARY_DIR}/${target}_assets_copied.stamp")

    add_custom_command(
        OUTPUT "${copy_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/cooked"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${out_dir}" "$<TARGET_FILE_DIR:${target}>/cooked"
        COMMAND "${CMAKE_COMMAND}" -E touch "${copy_stamp}"
        DEPENDS "${stamp}"
        COMMENT "Copying cooked assets for ${target}"
        VERBATIM
        COMMAND_EXPAND_LISTS)

    set(copy_target ${target}_copy_assets)
    add_custom_target(${copy_target} DEPENDS "${copy_stamp}")
    add_dependencies(${target} ${copy_target})
endfunction()
