# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

set(tdlib_loc ${third_party_loc}/td)
if (NOT EXISTS "${tdlib_loc}/CMakeLists.txt")
    message(FATAL_ERROR "TDLib is missing. Run git submodule update --init --recursive.")
endif()

function(telegram_add_tdlib)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTING OFF)
    set(TD_E2E_ONLY OFF)
    set(TD_ENABLE_JNI OFF)
    set(TD_ENABLE_DOTNET OFF)
    set(TD_GENERATE_SOURCE_FILES OFF)
    set(TD_INSTALL_STATIC_LIBRARIES OFF)
    set(TD_INSTALL_SHARED_LIBRARIES OFF)
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_CRYPTO_LIBRARY desktop-app::external_openssl_crypto)
    set(ZLIB_FOUND TRUE)
    set(ZLIB_LIBRARIES desktop-app::external_zlib)
    if (NOT DESKTOP_APP_USE_PACKAGED AND (WIN32 OR APPLE))
        set(OPENSSL_INCLUDE_DIR ${libs_loc}/openssl3/include)
    endif()
    if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
        set(ZLIB_INCLUDE_DIR ${libs_loc}/zlib)
    endif()
    if (APPLE AND DESKTOP_APP_MAC_ARCH)
        set(CMAKE_OSX_ARCHITECTURES ${DESKTOP_APP_MAC_ARCH})
    endif()
    add_subdirectory(${tdlib_loc} ${CMAKE_BINARY_DIR}/tdlib EXCLUDE_FROM_ALL)
endfunction()

telegram_add_tdlib()

add_library(external_td INTERFACE)
add_library(desktop-app::external_td ALIAS external_td)
target_link_libraries(external_td INTERFACE Td::TdStatic)

add_library(external_tde2e INTERFACE)
add_library(desktop-app::external_tde2e ALIAS external_tde2e)
target_link_libraries(external_tde2e INTERFACE tde2e)
