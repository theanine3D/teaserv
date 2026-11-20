# An empty drop folder belongs beside each server executable and in source-based installations.
function(teaserv_create_modpack_folder target)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/system/caelum-modpacks")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/system/caelum-modpacks"
        COMMENT "Creating the local Caelum modpack folder")
    install(DIRECTORY DESTINATION bin/system/caelum-modpacks)
endfunction()
