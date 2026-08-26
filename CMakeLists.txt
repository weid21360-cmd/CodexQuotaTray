cmake_minimum_required(VERSION 3.21)
project(CodexQuotaTray VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "CodexQuotaTray is a Windows-only application")
endif()

add_library(cqt_core STATIC
  src/json.cpp
  src/settings.cpp
  src/usage_parser.cpp
  src/usage_history.cpp
  src/codex_client.cpp
)
target_include_directories(cqt_core PUBLIC src)
target_compile_definitions(cqt_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(cqt_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")
target_link_libraries(cqt_core PUBLIC shlwapi shell32 ole32 advapi32)

add_executable(CodexQuotaTray WIN32
  src/main.cpp
  src/app.cpp
  src/ui.cpp
  src/tray.cpp
  src/resources.rc
)
target_include_directories(CodexQuotaTray PRIVATE src)
target_compile_definitions(CodexQuotaTray PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 CQT_VERSION=L\"${PROJECT_VERSION}\")
target_compile_options(CodexQuotaTray PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")
target_link_libraries(CodexQuotaTray PRIVATE cqt_core d2d1 dwrite dwmapi comdlg32 gdi32 user32 shell32 shlwapi ole32 advapi32)
if(MSVC)
  target_link_options(CodexQuotaTray PRIVATE /MANIFEST:NO)
endif()
set_target_properties(CodexQuotaTray PROPERTIES
  OUTPUT_NAME "CodexQuotaTray"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(cqt_tests
    tests/test_main.cpp
  )
  target_link_libraries(cqt_tests PRIVATE cqt_core)
  target_compile_definitions(cqt_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(cqt_tests PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")
  add_test(NAME cqt_tests COMMAND cqt_tests)

  add_executable(cqt_fake_app_server tests/fake_app_server.cpp)
  target_link_libraries(cqt_fake_app_server PRIVATE cqt_core)
  target_compile_definitions(cqt_fake_app_server PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(cqt_fake_app_server PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")

  add_executable(cqt_client_tests tests/client_test.cpp)
  target_link_libraries(cqt_client_tests PRIVATE cqt_core)
  target_compile_definitions(cqt_client_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(cqt_client_tests PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")
  add_test(NAME cqt_client_tests COMMAND cqt_client_tests $<TARGET_FILE:cqt_fake_app_server>)
endif()

install(TARGETS CodexQuotaTray RUNTIME DESTINATION .)
