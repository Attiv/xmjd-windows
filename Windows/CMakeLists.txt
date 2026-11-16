cmake_minimum_required(VERSION 3.24)

set(XMJD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XMJD_ROOT}/VERSION")
file(STRINGS "${XMJD_ROOT}/VERSION" XMJD_VERSION_LINES)
foreach(_line IN LISTS XMJD_VERSION_LINES)
  if(NOT _line MATCHES "^(MAJOR|MINOR|PATCH|BUILD)=([0-9]+)$")
    message(FATAL_ERROR "Windows/VERSION must contain numeric MAJOR, MINOR, PATCH and BUILD fields")
  endif()
  set(_part "${CMAKE_MATCH_1}")
  set(_value "${CMAKE_MATCH_2}")
  if(DEFINED XMJD_VERSION_${_part})
    message(FATAL_ERROR "Windows/VERSION contains duplicate ${_part}")
  endif()
  set(XMJD_VERSION_${_part} "${_value}")
endforeach()
foreach(_part MAJOR MINOR PATCH BUILD)
  if(NOT DEFINED XMJD_VERSION_${_part} OR XMJD_VERSION_${_part} GREATER 65535)
    message(FATAL_ERROR "Windows/VERSION ${_part} must be present and at most 65535")
  endif()
endforeach()
set(XMJD_VERSION_STRING "${XMJD_VERSION_MAJOR}.${XMJD_VERSION_MINOR}.${XMJD_VERSION_PATCH}")
set(XMJD_VERSION_FULL "${XMJD_VERSION_STRING}+${XMJD_VERSION_BUILD}")
project(XMJDWindows VERSION "${XMJD_VERSION_STRING}" LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W4 /EHsc /permissive-)
endif()

set(XMJD_VERSION_WIDE "L\"${XMJD_VERSION_FULL}\"")
set(XMJD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${XMJD_GENERATED_DIR}")
configure_file(cmake/Version.h.in "${XMJD_GENERATED_DIR}/Version.h" @ONLY)

add_library(xmjd_protocol STATIC
  Native/Protocol/Protocol.cpp
  Native/Protocol/Broker.cpp)
target_include_directories(xmjd_protocol PUBLIC
  "${XMJD_ROOT}/Native/Protocol")
target_compile_definitions(xmjd_protocol PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

if(BUILD_TESTING)
  add_executable(ProtocolTests Tests/ProtocolTests.cpp)
  target_link_libraries(ProtocolTests PRIVATE xmjd_protocol)
  add_test(NAME ProtocolTests COMMAND ProtocolTests)
endif()

file(READ "${XMJD_ROOT}/librime.lock.json" XMJD_RIME_LOCK)
file(SHA256 "${XMJD_ROOT}/librime.lock.json" XMJD_LOCK_SHA256)
string(JSON XMJD_SCHEME_PATH GET "${XMJD_RIME_LOCK}" scheme path)
string(JSON XMJD_SCHEME_SHA256 GET "${XMJD_RIME_LOCK}" scheme sha256)
set(XMJD_RESOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/resources")
set(XMJD_SCHEME_ARCHIVE "${XMJD_ROOT}/${XMJD_SCHEME_PATH}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XMJD_ROOT}/librime.lock.json" "${XMJD_SCHEME_ARCHIVE}")
file(SHA256 "${XMJD_SCHEME_ARCHIVE}" XMJD_SCHEME_ACTUAL_SHA256)
if(NOT XMJD_SCHEME_ACTUAL_SHA256 STREQUAL XMJD_SCHEME_SHA256)
  message(FATAL_ERROR "Scheme archive SHA-256 differs from librime.lock.json")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar tf "${XMJD_SCHEME_ARCHIVE}"
  OUTPUT_VARIABLE XMJD_SCHEME_ENTRIES RESULT_VARIABLE XMJD_SCHEME_LIST_RESULT)
if(NOT XMJD_SCHEME_LIST_RESULT EQUAL 0)
  message(FATAL_ERROR "Cannot list the pinned scheme archive")
endif()
string(REPLACE "\n" ";" XMJD_SCHEME_ENTRIES "${XMJD_SCHEME_ENTRIES}")
foreach(_entry IN LISTS XMJD_SCHEME_ENTRIES)
  if(_entry MATCHES "^/|\\\\|:|(^|/)\\.\\.(/|$)|(^|/)\\.(/|$)")
    message(FATAL_ERROR "Unsafe scheme archive path: ${_entry}")
  endif()
endforeach()
set(XMJD_SCHEME_SELECTED_ENTRIES ${XMJD_SCHEME_ENTRIES})
list(FILTER XMJD_SCHEME_SELECTED_ENTRIES INCLUDE REGEX
  "^[^/]+[.](schema|dict)[.]yaml$|^(default[.]yaml|default[.]custom[.]yaml|xmjd6[.]custom[.]yaml|xmjd6[.]symbols[.]yaml|zzc_state/char_parts[.]tsv)$|^(lua|opencc)/.+[^/]$")
if(NOT XMJD_SCHEME_SELECTED_ENTRIES)
  message(FATAL_ERROR "Pinned scheme archive contains no distributable resources")
endif()
file(REMOVE_RECURSE "${XMJD_RESOURCE_ROOT}/Rime")
file(ARCHIVE_EXTRACT INPUT "${XMJD_SCHEME_ARCHIVE}"
  DESTINATION "${XMJD_RESOURCE_ROOT}/Rime" PATTERNS ${XMJD_SCHEME_SELECTED_ENTRIES})
if(NOT EXISTS "${XMJD_RESOURCE_ROOT}/Rime/default.yaml")
  file(WRITE "${XMJD_RESOURCE_ROOT}/Rime/default.yaml" [=[config_version: '1.0'
schema_list:
  - schema: xmjd6
menu:
  page_size: 9
switcher:
  caption: 〔方案選單〕
  hotkeys:
    - F4
  save_options:
    - full_shape
    - ascii_punct
    - simplification
    - zh_simp
    - jffh
    - mars
    - tofu
    - emoji_cn
    - wxw_hint
    - completion
    - embeded_cands
key_binder:
  bindings:
    - { when: has_menu, accept: semicolon, send: 2 }
    - { when: has_menu, accept: apostrophe, send: 3 }
punctuator:
  full_shape:
    ' ': { commit: '　' }
    ',': { commit: ， }
    '.': { commit: 。 }
    '<': [ 《, 〈, «, ‹ ]
    '>': [ 》, 〉, », › ]
    '/': [ ／, ÷ ]
    '?': { commit: ？ }
    ';': { commit: ； }
    ':': { commit: ： }
    "'": { pair: [ ‘, ’ ] }
    '"': { pair: [ “, ” ] }
    '\\': [ 、, ＼ ]
    '|': [ ·, ｜, '§', '¦' ]
    '`': ｀
    '~': ～
    '!': { commit: ！ }
    '@': [ ＠, ☯ ]
    '#': [ ＃, ⌐ ]
    '%': [ ％, '°', '℃' ]
    '$': [ ￥, '$', '€', '£', '¥', '¢', '¤' ]
    '^': { commit: …… }
    '&': ＆
    '*': [ ＊, ·, ・, ×, ※, ❂ ]
    '(': （
    ')': ）
    '-': －
    '_': ——
    '+': ＋
    '=': ＝
    '[': [ 「, 【, 〔, ［ ]
    ']': [ 」, 】, 〕, ］ ]
    '{': [ 『, 〖, ｛ ]
    '}': [ 』, 〗, ｝ ]
  half_shape:
    ',': { commit: ， }
    '.': { commit: 。 }
    '<': [ 《, 〈, «, ‹ ]
    '>': [ 》, 〉, », › ]
    '/': [ 、, '/', ／, ÷ ]
    '?': { commit: ？ }
    ';': { commit: ； }
    ':': { commit: ： }
    "'": { pair: [ ‘, ’ ] }
    '"': { pair: [ “, ” ] }
    '\\': [ 、, '\\', ＼ ]
    '|': [ ·, '|', ｜, '§', '¦' ]
    '`': '`'
    '~': [ '~', ～ ]
    '!': { commit: ！ }
    '@': '@'
    '#': '#'
    '%': [ '%', ％, '°', '℃' ]
    '$': [ ￥, '$', '€', '£', '¥', '¢', '¤' ]
    '^': { commit: …… }
    '&': '&'
    '*': [ '*', ＊, ·, ・, ×, ※, ❂ ]
    '(': （
    ')': ）
    '-': '-'
    '_': ——
    '+': '+'
    '=': '='
    '[': [ 「, 【, 〔, ［ ]
    ']': [ 」, 】, 〕, ］ ]
    '{': [ 『, 〖, ｛ ]
    '}': [ 』, 〗, ｝ ]
]=])
endif()
if(NOT EXISTS "${XMJD_RESOURCE_ROOT}/Rime/default.custom.yaml")
  message(FATAL_ERROR "Pinned scheme must include default.custom.yaml")
endif()
file(GLOB_RECURSE XMJD_STAGED_RESOURCES LIST_DIRECTORIES TRUE "${XMJD_RESOURCE_ROOT}/Rime/*")
foreach(_resource IN LISTS XMJD_STAGED_RESOURCES)
  if(IS_SYMLINK "${_resource}")
    message(FATAL_ERROR "Scheme resources must not contain links: ${_resource}")
  endif()
endforeach()

if(NOT WIN32)
  return()
endif()
if(NOT MSVC)
  message(FATAL_ERROR "The Windows IME targets require MSVC")
endif()
if(NOT CMAKE_VS_PLATFORM_NAME MATCHES "^(x64|Win32)$")
  message(FATAL_ERROR "Use the Visual Studio 2022 generator with -A x64 or -A Win32")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
if(TARGET ProtocolTests)
  set_target_properties(ProtocolTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(XMJD_ARCH x64)
else()
  set(XMJD_ARCH x86)
endif()
enable_language(RC)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_library(xmjd_windows_common STATIC Native/Common/Pipe.cpp)
target_include_directories(xmjd_windows_common PUBLIC
  "${XMJD_ROOT}/Native/Common" "${XMJD_ROOT}/Native/Protocol")
target_link_libraries(xmjd_windows_common PUBLIC xmjd_protocol advapi32 shell32 uuid)
file(GLOB XMJD_TIP_SOURCES CONFIGURE_DEPENDS "${XMJD_ROOT}/Native/Tip/*.cpp")
file(GLOB XMJD_TIP_RESOURCES CONFIGURE_DEPENDS "${XMJD_ROOT}/Native/Tip/*.rc")
file(GLOB XMJD_TIP_DEFINITIONS CONFIGURE_DEPENDS "${XMJD_ROOT}/Native/Tip/*.def")
if(NOT XMJD_TIP_SOURCES)
  message(FATAL_ERROR "No TSF sources found in Native/Tip")
endif()
set(XMJD_ICON "${XMJD_ROOT}/Resources/XMJD.ico")
if(NOT EXISTS "${XMJD_ICON}")
  message(FATAL_ERROR "Missing Windows product icon: ${XMJD_ICON}")
endif()
file(TO_CMAKE_PATH "${XMJD_ICON}" XMJD_ICON_RC)
configure_file(cmake/Version.rc.in "${XMJD_GENERATED_DIR}/Version.rc" @ONLY)

add_library(XMJDTip SHARED
  ${XMJD_TIP_SOURCES}
  ${XMJD_TIP_RESOURCES}
  ${XMJD_TIP_DEFINITIONS}
  "${XMJD_GENERATED_DIR}/Version.rc")
target_include_directories(XMJDTip PRIVATE "${XMJD_GENERATED_DIR}")
target_link_libraries(XMJDTip PRIVATE
  xmjd_windows_common
  ole32 oleaut32 uuid advapi32 user32 gdi32 shell32 shlwapi)
set_target_properties(XMJDTip PROPERTIES
  OUTPUT_NAME XMJDTip
  PREFIX ""
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Tip/${XMJD_ARCH}")
install(TARGETS XMJDTip
  RUNTIME DESTINATION "Tip/${XMJD_ARCH}"
  LIBRARY DESTINATION "Tip/${XMJD_ARCH}")

if(XMJD_ARCH STREQUAL "x64")
  string(JSON XMJD_RIME_VERSION GET "${XMJD_RIME_LOCK}" version)
  string(JSON XMJD_RIME_COMMIT GET "${XMJD_RIME_LOCK}" commit)
  string(JSON XMJD_RIME_ENGINE_SHA256 GET "${XMJD_RIME_LOCK}" architectures x64 engine sha256)
  string(JSON XMJD_RIME_DEPENDENCIES_SHA256 GET "${XMJD_RIME_LOCK}" architectures x64 dependencies sha256)
  string(JSON XMJD_RIME_LUA_COMMIT GET "${XMJD_RIME_LOCK}" plugins librime-lua commit)
  string(JSON XMJD_RIME_LUA_VERSION GET "${XMJD_RIME_LOCK}" plugins librime-lua luaVersion)
  set(XMJD_RIME_ROOT "${XMJD_ROOT}/.deps/librime/${XMJD_RIME_VERSION}/x64"
      CACHE PATH "Verified x64 librime package prepared by scripts/prepare-deps.ps1")
  set(XMJD_RIME_INCLUDE_DIR "${XMJD_RIME_ROOT}/include")
  foreach(_required include/rime_api.h include/rime_api_stdbool.h bin/rime.dll
      xmjd-librime-lock.json version-info.txt share/opencc/s2t.json share/opencc/STCharacters.ocd2 licenses/librime-LICENSE)
    if(NOT EXISTS "${XMJD_RIME_ROOT}/${_required}")
      message(FATAL_ERROR "Missing verified dependency ${_required}; run scripts/prepare-deps.ps1 -Architecture x64")
    endif()
  endforeach()
  file(READ "${XMJD_RIME_ROOT}/xmjd-librime-lock.json" XMJD_RIME_MARKER)
  foreach(_field formatVersion lockSha256 version commit architecture engineSha256 dependenciesSha256 luaPluginCommit luaVersion)
    string(JSON XMJD_MARKER_${_field} GET "${XMJD_RIME_MARKER}" "${_field}")
  endforeach()
  if(NOT XMJD_MARKER_formatVersion EQUAL 2 OR
     NOT XMJD_MARKER_lockSha256 STREQUAL XMJD_LOCK_SHA256 OR
     NOT XMJD_MARKER_version STREQUAL XMJD_RIME_VERSION OR
     NOT XMJD_MARKER_commit STREQUAL XMJD_RIME_COMMIT OR
     NOT XMJD_MARKER_architecture STREQUAL "x64" OR
     NOT XMJD_MARKER_engineSha256 STREQUAL XMJD_RIME_ENGINE_SHA256 OR
     NOT XMJD_MARKER_dependenciesSha256 STREQUAL XMJD_RIME_DEPENDENCIES_SHA256 OR
     NOT XMJD_MARKER_luaPluginCommit STREQUAL XMJD_RIME_LUA_COMMIT OR
     NOT XMJD_MARKER_luaVersion STREQUAL XMJD_RIME_LUA_VERSION)
    message(FATAL_ERROR "Prepared dependency metadata differs from librime.lock.json")
  endif()
  file(GLOB_RECURSE XMJD_RIME_TREE CONFIGURE_DEPENDS LIST_DIRECTORIES TRUE "${XMJD_RIME_ROOT}/*")
  foreach(_path IN LISTS XMJD_RIME_TREE)
    if(IS_SYMLINK "${_path}")
      message(FATAL_ERROR "Dependency links are not allowed: ${_path}")
    endif()
  endforeach()
  file(GLOB_RECURSE XMJD_RIME_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE RELATIVE "${XMJD_RIME_ROOT}" "${XMJD_RIME_ROOT}/*")
  list(REMOVE_ITEM XMJD_RIME_FILES xmjd-librime-lock.json)
  list(LENGTH XMJD_RIME_FILES XMJD_RIME_FILE_COUNT)
  string(JSON XMJD_INVENTORY_COUNT LENGTH "${XMJD_RIME_MARKER}" files)
  if(NOT XMJD_INVENTORY_COUNT EQUAL XMJD_RIME_FILE_COUNT OR XMJD_INVENTORY_COUNT EQUAL 0)
    message(FATAL_ERROR "Prepared dependency inventory count differs")
  endif()
  math(EXPR _last "${XMJD_INVENTORY_COUNT} - 1")
  set(_inventory_paths)
  foreach(_index RANGE 0 ${_last})
    string(JSON _path GET "${XMJD_RIME_MARKER}" files ${_index} path)
    string(JSON _size GET "${XMJD_RIME_MARKER}" files ${_index} size)
    string(JSON _hash GET "${XMJD_RIME_MARKER}" files ${_index} sha256)
    if(NOT _path MATCHES "^[A-Za-z0-9_./-]+$" OR _path MATCHES "^/|(^|/)[.][.]?(/|$)" OR NOT _path IN_LIST XMJD_RIME_FILES)
      message(FATAL_ERROR "Invalid dependency inventory path: ${_path}")
    endif()
    string(TOLOWER "${_path}" _folded_path)
    if(_folded_path IN_LIST _inventory_paths)
      message(FATAL_ERROR "Duplicate dependency inventory path: ${_path}")
    endif()
    list(APPEND _inventory_paths "${_folded_path}")
    file(SHA256 "${XMJD_RIME_ROOT}/${_path}" _actual_hash)
    file(SIZE "${XMJD_RIME_ROOT}/${_path}" _actual_size)
    if(NOT _hash STREQUAL _actual_hash OR NOT _size EQUAL _actual_size)
      message(FATAL_ERROR "Prepared dependency content differs: ${_path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XMJD_RIME_ROOT}/${_path}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XMJD_RIME_ROOT}/xmjd-librime-lock.json")
  string(JSON _license_count LENGTH "${XMJD_RIME_LOCK}" licenses)
  math(EXPR _last_license "${_license_count} - 1")
  foreach(_index RANGE 0 ${_last_license})
    string(JSON _name GET "${XMJD_RIME_LOCK}" licenses ${_index} name)
    string(JSON _hash GET "${XMJD_RIME_LOCK}" licenses ${_index} sha256)
    file(SHA256 "${XMJD_RIME_ROOT}/licenses/${_name}" _actual_hash)
    if(NOT _hash STREQUAL _actual_hash)
      message(FATAL_ERROR "Dependency license differs from lock: ${_name}")
    endif()
  endforeach()
  file(GLOB XMJD_RIME_DLLS CONFIGURE_DEPENDS "${XMJD_RIME_ROOT}/bin/*.dll")
  set(XMJD_RIME_RESOURCES "${XMJD_RESOURCE_ROOT}/Rime")
  file(GLOB_RECURSE XMJD_OPENCC_FILES LIST_DIRECTORIES FALSE RELATIVE "${XMJD_RIME_ROOT}/share/opencc" "${XMJD_RIME_ROOT}/share/opencc/*")
  foreach(_file IN LISTS XMJD_OPENCC_FILES)
    if(EXISTS "${XMJD_RIME_RESOURCES}/opencc/${_file}")
      file(SHA256 "${XMJD_RIME_ROOT}/share/opencc/${_file}" _dependency_hash)
      file(SHA256 "${XMJD_RIME_RESOURCES}/opencc/${_file}" _scheme_hash)
      if(NOT _dependency_hash STREQUAL _scheme_hash)
        message(FATAL_ERROR "OpenCC data collision: ${_file}")
      endif()
    endif()
  endforeach()
  file(COPY "${XMJD_RIME_ROOT}/share/opencc" DESTINATION "${XMJD_RIME_RESOURCES}")
  if(NOT EXISTS "${XMJD_RIME_RESOURCES}/xmjd6.schema.yaml" OR NOT IS_DIRECTORY "${XMJD_RIME_RESOURCES}/lua")
    message(FATAL_ERROR "Missing staged Rime resources in ${XMJD_RIME_RESOURCES}")
  endif()
  message(STATUS "Dependency content verified; Host must still pass find_module(lua) at runtime")
  configure_file(cmake/HostVersion.rc.in "${XMJD_GENERATED_DIR}/HostVersion.rc" @ONLY)
  add_executable(XMJDHost
    Native/Host/Main.cpp
    Native/Host/RimeRuntime.cpp
    "${XMJD_GENERATED_DIR}/HostVersion.rc")
  target_include_directories(XMJDHost PRIVATE "${XMJD_RIME_INCLUDE_DIR}" "${XMJD_GENERATED_DIR}")
  target_link_libraries(XMJDHost PRIVATE xmjd_windows_common ole32 advapi32 shell32)
  add_custom_command(TARGET XMJDHost POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${XMJD_RIME_DLLS} "$<TARGET_FILE_DIR:XMJDHost>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${XMJD_RIME_RESOURCES}" "$<TARGET_FILE_DIR:XMJDHost>/Resources/Rime"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${XMJD_RIME_ROOT}/licenses" "$<TARGET_FILE_DIR:XMJDHost>/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${XMJD_RIME_ROOT}/share" "$<TARGET_FILE_DIR:XMJDHost>/share"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XMJD_RIME_ROOT}/xmjd-librime-lock.json" "${XMJD_ROOT}/librime.lock.json"
            "${XMJD_RIME_ROOT}/version-info.txt" "$<TARGET_FILE_DIR:XMJDHost>"
    VERBATIM)
  if(BUILD_TESTING)
    add_executable(RimeRuntimeTests
      Tests/RimeRuntimeTests.cpp
      Native/Host/RimeRuntime.cpp)
    target_include_directories(RimeRuntimeTests PRIVATE "${XMJD_RIME_INCLUDE_DIR}" "${XMJD_GENERATED_DIR}")
    target_link_libraries(RimeRuntimeTests PRIVATE xmjd_windows_common ole32)
    add_dependencies(RimeRuntimeTests XMJDHost)
    # 真实运行时会写入当前用户产品目录，仅在 CI 注册，并由测试拒绝已有数据。
    if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
      add_test(NAME RimeRuntimeTests COMMAND RimeRuntimeTests "$<TARGET_FILE_DIR:XMJDHost>")
      set_tests_properties(RimeRuntimeTests PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 LABELS "integration;librime")
    endif()
  endif()
  install(TARGETS XMJDHost RUNTIME DESTINATION .)
  install(FILES ${XMJD_RIME_DLLS}
    "${XMJD_RIME_ROOT}/xmjd-librime-lock.json" "${XMJD_ROOT}/librime.lock.json"
    "${XMJD_RIME_ROOT}/version-info.txt" "${XMJD_ROOT}/VERSION" DESTINATION .)
  install(DIRECTORY "${XMJD_RIME_RESOURCES}" DESTINATION Resources)
  install(DIRECTORY "${XMJD_RIME_ROOT}/licenses" "${XMJD_RIME_ROOT}/share" DESTINATION .)
  install(FILES "${XMJD_ROOT}/README.md" "${XMJD_ROOT}/IMPLEMENTATION_PLAN.md" DESTINATION .)
  install(FILES "${XMJD_ROOT}/scripts/build.ps1" "${XMJD_ROOT}/scripts/install.ps1"
    "${XMJD_ROOT}/scripts/register.ps1" "${XMJD_ROOT}/scripts/unregister.ps1" DESTINATION scripts)
else()
  message(STATUS "x86 TIP uses the x64 Host over IPC; no x86 librime package is required")
endif()

if(BUILD_TESTING)
  add_executable(XMJDTipComTests Tests/TipComTests.cpp)
  target_link_libraries(XMJDTipComTests PRIVATE ole32 uuid)
  add_dependencies(XMJDTipComTests XMJDTip)
  add_test(NAME XMJDTipComTests COMMAND XMJDTipComTests "$<TARGET_FILE:XMJDTip>")
  add_executable(EditorHarness WIN32 Tests/EditorHarness.cpp)
  target_link_libraries(EditorHarness PRIVATE user32)
  add_test(NAME EditorHarness COMMAND EditorHarness --smoke)
endif()
