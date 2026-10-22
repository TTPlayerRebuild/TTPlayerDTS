cmake_minimum_required(VERSION 3.24)
project(TtpDts LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_dts requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
include(cmake/codec.cmake)
include(cmake/version.cmake)
add_library(ttp_dts SHARED src/plugin.cpp src/reader.cpp src/ttp_dts.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_dts ttp_dts_version)
target_include_directories(ttp_dts PUBLIC include PRIVATE src)
target_compile_definitions(ttp_dts PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_dts PRIVATE /W4 /permissive- /EHsc /GR- /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_dts PRIVATE codec ole32 uuid shlwapi)
set_target_properties(ttp_dts PROPERTIES PREFIX "" OUTPUT_NAME "ttp_dts")
add_custom_command(TARGET ttp_dts POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_dts>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_dts>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_dts RUNTIME DESTINATION AddIn COMPONENT Runtime)

