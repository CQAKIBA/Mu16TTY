cmake_minimum_required(VERSION 3.21)
project(mu16tty_modem VERSION 0.1.0 LANGUAGES C)

add_library(mu16tty_modem SHARED
    src/mu16tty_modem.c
    src/mu16tty_metrics.c
    src/mu16tty_mid_recovery.c
    src/mu16tty_crc.c
    src/mu16tty_coding.c
    src/mu16tty_fec.c
    src/mu16tty_packet.c
    src/mu16tty_resampler.c
    src/mu16tty_audio_monitor.c
)
target_include_directories(mu16tty_modem PUBLIC include)
target_compile_features(mu16tty_modem PRIVATE c_std_11)
target_compile_definitions(mu16tty_modem PRIVATE MU16TTY_MODEM_BUILD)

if(WIN32)
    target_sources(mu16tty_modem PRIVATE src/mu16tty_windows_ptt.c src/mu16tty_windows_audio.c)
    target_link_libraries(mu16tty_modem PRIVATE setupapi advapi32)
    add_executable(mu16tty_station src/mu16tty_station.c src/mu16tty_station_http.c
        src/mu16tty_station_settings.c)
    target_link_libraries(mu16tty_station PRIVATE mu16tty_modem bcrypt ws2_32 winsqlite3 shell32 user32)
    target_compile_features(mu16tty_station PRIVATE c_std_11)
    set_target_properties(mu16tty_station PROPERTIES OUTPUT_NAME "Mu16TTY-native")
    if(MSVC)
        target_compile_options(mu16tty_station PRIVATE /W4 /WX /permissive-)
    elseif(MINGW)
        target_link_options(mu16tty_station PRIVATE -municode)
    endif()
endif()

if(MSVC)
    target_compile_options(mu16tty_modem PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(mu16tty_modem PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_library(mu16tty_coding_test SHARED
    tests/mu16tty_coding_test.c
    src/mu16tty_crc.c
    src/mu16tty_coding.c
    src/mu16tty_fec.c
    src/mu16tty_packet.c
    src/mu16tty_mid_recovery.c
)
target_include_directories(mu16tty_coding_test PRIVATE include src)
target_compile_features(mu16tty_coding_test PRIVATE c_std_11)
target_compile_definitions(mu16tty_coding_test PRIVATE MU16TTY_MODEM_BUILD)
if(MSVC)
    target_compile_options(mu16tty_coding_test PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(mu16tty_coding_test PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(mu16tty_core_test ../tests/test_core.c)
    target_link_libraries(mu16tty_core_test PRIVATE mu16tty_modem)
    target_compile_features(mu16tty_core_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_definitions(mu16tty_core_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME core_roundtrip COMMAND mu16tty_core_test
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/data/packet-v1.bin")
endif()
