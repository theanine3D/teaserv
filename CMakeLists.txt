cmake_minimum_required(VERSION 3.22)
set(CMAKE_POLICY_DEFAULT_CMP0110 NEW)



# Project setup

project(newserv)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -Wno-address-of-packed-member)
endif()



# Library search

option(NEWSERV_ENABLE_EXTENSIONS "Build the opt-in Teaserv Lua extension host" OFF)
option(NEWSERV_EXTENSIONS_TESTS_ONLY "Build extension runtime tests without Newserv dependencies" OFF)
set(NEWSERV_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Data checkout used by server integration tests")
if(NEWSERV_ENABLE_EXTENSIONS OR NEWSERV_EXTENSIONS_TESTS_ONLY)
    enable_testing()
    add_subdirectory(src/Extensions)
endif()
if(NEWSERV_EXTENSIONS_TESTS_ONLY)
    return()
endif()

find_path(ASIO_INCLUDE_DIR NAMES asio.hpp HINTS "${WINDOWS_ENV}/include" REQUIRED)
if(WIN32)
    find_path(Iconv_INCLUDE_DIRS NAMES iconv.h HINTS "${WINDOWS_ENV}/include" REQUIRED)
    find_library(Iconv_LIBRARIES NAMES iconv HINTS "${WINDOWS_ENV}/lib" REQUIRED)
else()
    find_package(Iconv REQUIRED)
endif()
find_package(phosg REQUIRED)
find_package(resource_file REQUIRED)



# Git metadata

find_package(Git QUIET)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/Revision.cc ${CMAKE_CURRENT_SOURCE_DIR}/src/__Revision__.cc
    COMMAND ${CMAKE_COMMAND}
        "-DREVISION_OUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/Revision.cc"
        "-DREVISION_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateRevision.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_custom_target(
    newserv-Revision-cc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/Revision.cc ${CMAKE_CURRENT_SOURCE_DIR}/src/__Revision__.cc
)



# Executable definition

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Revision.cc
    src/Account.cc
    src/AddressTranslator.cc
    src/AFSArchive.cc
    src/AsyncHTTPServer.cc
    src/AsyncUtils.cc
    src/BattleParamsIndex.cc
    src/BMLArchive.cc
    src/Channel.cc
    src/ChatCommands.cc
    src/ChoiceSearch.cc
    src/Client.cc
    src/ClientFunctionIndex.cc
    src/CommandCensorData.cc
    src/CommonItemSet.cc
    src/Compression.cc
    src/DCSerialNumbers.cc
    src/DNSServer.cc
    src/DOLFileIndex.cc
    src/DataIndex.cc
    src/DownloadSession.cc
    src/EnemyType.cc
    src/Episode3/AssistServer.cc
    src/Episode3/BattleRecord.cc
    src/Episode3/Card.cc
    src/Episode3/CardSpecial.cc
    src/Episode3/DataIndexes.cc
    src/Episode3/DeckState.cc
    src/Episode3/MapState.cc
    src/Episode3/PlayerState.cc
    src/Episode3/PlayerStateSubordinates.cc
    src/Episode3/RulerServer.cc
    src/Episode3/Server.cc
    src/Episode3/Tournament.cc
    src/GameServer.cc
    src/GSLArchive.cc
    src/HTTPServer.cc
    src/ImageEncoder.cc
    src/IPFrameInfo.cc
    src/IPStackSimulator.cc
    src/IPV4RangeSet.cc
    src/ItemCreator.cc
    src/ItemData.cc
    src/ItemNameIndex.cc
    src/ItemParameterTable.cc
    src/Items.cc
    src/ItemTranslationTable.cc
    src/LevelTable.cc
    src/Lobby.cc
    src/Loggers.cc
    src/MagMetadataTable.cc
    src/Main.cc
    src/Map.cc
    src/Menu.cc
    src/NetworkAddresses.cc
    src/PatchDownloadSession.cc
    src/PatchFileIndex.cc
    src/PlayerInventory.cc
    src/PlayerSubordinates.cc
    src/PPKArchive.cc
    src/ProxyCommands.cc
    src/ProxySession.cc
    src/PSOEncryption.cc
    src/PSOGCObjectGraph.cc
    src/PSOProtocol.cc
    src/Quest.cc
    src/QuestMetadata.cc
    src/QuestScript.cc
    src/RareItemSet.cc
    src/ReceiveCommands.cc
    src/ReceiveSubcommands.cc
    src/ReplaySession.cc
    src/SaveFileFormats.cc
    src/SendCommands.cc
    src/ServerShell.cc
    src/ServerState.cc
    src/ShellCommands.cc
    src/ShopRandomSets.cc
    src/SignalWatcher.cc
    src/StaticGameData.cc
    src/StaticTests.cc
    src/TeamIndex.cc
    src/TekkerAdjustmentSet.cc
    src/Text.cc
    src/TextIndex.cc
    src/Version.cc
    src/WordSelectTable.cc
)

add_executable(newserv ${SOURCES})
# Keep the upstream target name to minimize merge churn; brand the output file.
set_target_properties(newserv PROPERTIES OUTPUT_NAME teaserv)
if(NEWSERV_ENABLE_EXTENSIONS)
    target_sources(newserv PRIVATE src/Extensions/Manager.cc src/Extensions/NativeAssets.cc)
    target_compile_definitions(newserv PRIVATE NEWSERV_ENABLE_EXTENSIONS=1)
    target_link_libraries(newserv teaserv-extension-runtime)
    add_executable(extension-native-assets-tests
        tests/extensions/NativeAssetsTest.cc src/Extensions/NativeAssets.cc
        src/Compression.cc src/ImageEncoder.cc src/Text.cc)
    target_include_directories(extension-native-assets-tests PRIVATE ${Iconv_INCLUDE_DIRS})
    target_link_libraries(extension-native-assets-tests PRIVATE teaserv-extension-runtime phosg::phosg ${Iconv_LIBRARIES})
    add_test(NAME extension-native-assets COMMAND extension-native-assets-tests
        ${CMAKE_SOURCE_DIR}/system/extensions/lobby-decor/assets/checker.bmp
        ${CMAKE_BINARY_DIR}/lobby-media-fixtures)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(PHOSG_JSONFORMAT_EXECUTABLE NAMES jsonformat)
    if(Python3_Interpreter_FOUND AND PHOSG_JSONFORMAT_EXECUTABLE)
        add_test(NAME extension-host-smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/extensions/HostSmokeTest.py
            --server $<TARGET_FILE:newserv> --source ${NEWSERV_TEST_DATA_DIR}
            --build ${CMAKE_BINARY_DIR} --jsonformat ${PHOSG_JSONFORMAT_EXECUTABLE})
        set_tests_properties(extension-host-smoke PROPERTIES TIMEOUT 240 DEPENDS extension-native-assets)
        add_test(NAME extension-storage-smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/extensions/StorageSmokeTest.py
            --server $<TARGET_FILE:newserv> --source ${NEWSERV_TEST_DATA_DIR}
            --build ${CMAKE_BINARY_DIR} --jsonformat ${PHOSG_JSONFORMAT_EXECUTABLE})
        set_tests_properties(extension-storage-smoke PROPERTIES TIMEOUT 360)
        add_test(NAME extension-package-smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/extensions/PackageLifecycleSmokeTest.py
            --server $<TARGET_FILE:newserv> --source ${NEWSERV_TEST_DATA_DIR}
            --build ${CMAKE_BINARY_DIR} --jsonformat ${PHOSG_JSONFORMAT_EXECUTABLE})
        set_tests_properties(extension-package-smoke PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "Extension host smoke test requires Python 3 and phosg's jsonformat executable")
    endif()
endif()
target_include_directories(newserv PUBLIC ${ASIO_INCLUDE_DIR} ${Iconv_INCLUDE_DIRS})
target_link_libraries(newserv phosg::phosg ${Iconv_LIBRARIES} resource_file::resource_file)
if (WIN32)
    target_compile_definitions(newserv PUBLIC WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MINGW)
        # MinGW's unistd.h exposes _POSIX_THREADS. Without an explicit choice,
        # Asio's thread_pool layout depends on whether that header preceded
        # asio.hpp in each translation unit, violating the one-definition rule.
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("
            #include <unistd.h>
            #include <pthread.h>
            #if !defined(_POSIX_THREADS) || _POSIX_THREADS < 0
            #error POSIX threads are unavailable
            #endif
            int main() { return sizeof(pthread_t) == 0; }
        " NEWSERV_MINGW_HAS_PTHREADS)
        if(NEWSERV_MINGW_HAS_PTHREADS)
            target_compile_definitions(newserv PRIVATE ASIO_HAS_PTHREADS=1)
            target_link_libraries(newserv pthread)
        endif()
    endif()
    target_compile_options(newserv PRIVATE -Wa,-mbig-obj -Wno-mismatched-new-delete)
    option(NEWSERV_WINDOWS_STATIC_LINK "Use Newserv's static MinGW link options" ON)
    if(NEWSERV_WINDOWS_STATIC_LINK)
        target_link_options(newserv PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    target_link_libraries(newserv ws2_32 mswsock bcrypt iphlpapi)
    if(NEWSERV_ENABLE_EXTENSIONS)
        target_compile_options(extension-native-assets-tests PRIVATE -Wa,-mbig-obj -Wno-mismatched-new-delete)
    endif()
endif()
add_dependencies(newserv newserv-Revision-cc)

# if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
#     message("-- Enabling address sanitizer")
#     target_compile_options(newserv PRIVATE -fsanitize=address)
#     target_link_options(newserv PRIVATE -fsanitize=address)
# endif()
# if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
#     message("-- Enabling thread sanitizer")
#     target_compile_options(newserv PRIVATE -fsanitize=thread)
#     target_link_options(newserv PRIVATE -fsanitize=thread)
# endif()



# Test configuration

enable_testing()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME replay-line-endings COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/extensions/ReplayLineEndingsTest.py
        --server $<TARGET_FILE:newserv> --source ${NEWSERV_TEST_DATA_DIR} --build ${CMAKE_BINARY_DIR})
    set_tests_properties(replay-line-endings PROPERTIES TIMEOUT 150)
endif()

file(GLOB LOG_TEST_CASES ${CMAKE_SOURCE_DIR}/tests/*.test.txt)
list(TRANSFORM LOG_TEST_CASES PREPEND "--replay-log=" OUTPUT_VARIABLE LOG_REPLAY_ARGS)
add_test(
    NAME "log-replays"
    WORKING_DIRECTORY ${NEWSERV_TEST_DATA_DIR}
    COMMAND $<TARGET_FILE:newserv> --parallel --config=${CMAKE_SOURCE_DIR}/tests/config.json ${LOG_REPLAY_ARGS})

file(GLOB SCRIPT_TEST_CASES ${CMAKE_SOURCE_DIR}/tests/*.test.sh)
foreach(SCRIPT_TEST_CASE IN ITEMS ${SCRIPT_TEST_CASES})
    get_filename_component(SCRIPT_TEST_CASE_NAME ${SCRIPT_TEST_CASE} NAME)
    add_test(
        NAME ${SCRIPT_TEST_CASE_NAME}
        WORKING_DIRECTORY ${NEWSERV_TEST_DATA_DIR}
        COMMAND ${SCRIPT_TEST_CASE} $<TARGET_FILE:newserv>)
endforeach()



# Installation configuration

install(TARGETS newserv DESTINATION bin)
