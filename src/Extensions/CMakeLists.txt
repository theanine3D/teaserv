find_package(Lua 5.4 EXACT REQUIRED)

add_library(teaserv-extension-runtime STATIC Runtime.cc Scene.cc NativeMediaFormat.cc CommandArguments.cc CommandCatalog.cc Settings.cc LobbyView.cc GameLifecycle.cc)
target_include_directories(teaserv-extension-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(teaserv-extension-runtime PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(teaserv-extension-runtime PUBLIC ${LUA_LIBRARIES})

add_executable(extension-runtime-tests ../../tests/extensions/RuntimeTest.cc)
target_link_libraries(extension-runtime-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-runtime COMMAND extension-runtime-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/chat-tools/main.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/lobby-decor/main.lua)

add_executable(extension-scene-tests ../../tests/extensions/SceneTest.cc)
target_link_libraries(extension-scene-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-scenes COMMAND extension-scene-tests)

add_executable(extension-settings-tests ../../tests/extensions/SettingsTest.cc)
target_link_libraries(extension-settings-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-settings COMMAND extension-settings-tests)

add_executable(extension-lobby-tests ../../tests/extensions/LobbyViewTest.cc)
target_link_libraries(extension-lobby-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-lobby COMMAND extension-lobby-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/lobby-tools/main.lua)

add_executable(extension-package-tests ../../tests/extensions/PackageLifecycleTest.cc)
target_link_libraries(extension-package-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-packages COMMAND extension-package-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/package-audit/main.lua)

add_executable(extension-game-tests ../../tests/extensions/GameLifecycleTest.cc)
target_link_libraries(extension-game-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-games COMMAND extension-game-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/game-audit/main.lua)

add_executable(extension-quest-tests ../../tests/extensions/QuestLifecycleTest.cc)
target_link_libraries(extension-quest-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-quests COMMAND extension-quest-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/quest-audit/main.lua)

add_executable(extension-session-tests ../../tests/extensions/SessionLifecycleTest.cc)
target_link_libraries(extension-session-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-sessions COMMAND extension-session-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/session-audit/main.lua)

add_executable(extension-command-catalog-tests ../../tests/extensions/CommandCatalogTest.cc)
target_link_libraries(extension-command-catalog-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-command-catalog COMMAND extension-command-catalog-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/command-help/main.lua)

add_executable(extension-live-membership-tests ../../tests/extensions/LiveMembershipTest.cc)
target_link_libraries(extension-live-membership-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-live-membership COMMAND extension-live-membership-tests)

add_executable(extension-announcement-tests ../../tests/extensions/AnnouncementTest.cc)
target_link_libraries(extension-announcement-tests PRIVATE teaserv-extension-runtime)
add_test(NAME extension-announcements COMMAND extension-announcement-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/extensions/server-announcements/main.lua)

find_package(Python3 QUIET COMPONENTS Interpreter)
find_package(Git QUIET)
if(Python3_Interpreter_FOUND AND GIT_FOUND)
    add_test(NAME extension-upstream-audit COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/extensions/UpstreamAuditTest.py
        --git ${GIT_EXECUTABLE} --build ${CMAKE_BINARY_DIR})
endif()
