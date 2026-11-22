# Microsoft CPU runtime. Only the x64 Host performs inference.
set(_ort_version 1.24.3)
set(_ort_sha e4ab3b236c1b803e1e7167fe9a75caa1b82a2fd4ae82abbd130937c07517bcdb)
set(_ort_archive "${XMJD_ROOT}/.deps/cache/onnxruntime.${_ort_version}.nupkg")
set(XMJD_MSIME_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/msime-runtime")
if(NOT EXISTS "${_ort_archive}")
  file(MAKE_DIRECTORY "${XMJD_ROOT}/.deps/cache")
  file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${_ort_version}/microsoft.ml.onnxruntime.${_ort_version}.nupkg"
       "${_ort_archive}.download" EXPECTED_HASH SHA256=${_ort_sha} TLS_VERIFY ON TIMEOUT 180)
  file(RENAME "${_ort_archive}.download" "${_ort_archive}")
endif()
file(SHA256 "${_ort_archive}" _ort_actual)
if(NOT _ort_actual STREQUAL _ort_sha)
  message(FATAL_ERROR "ONNX Runtime package hash differs from the pinned version")
endif()
file(ARCHIVE_EXTRACT INPUT "${_ort_archive}" DESTINATION "${XMJD_MSIME_RUNTIME_DIR}"
  PATTERNS "runtimes/win-x64/native/*.dll" LICENSE ThirdPartyNotices.txt)
set(XMJD_MSIME_RUNTIME "${XMJD_MSIME_RUNTIME_DIR}/runtimes/win-x64/native")
