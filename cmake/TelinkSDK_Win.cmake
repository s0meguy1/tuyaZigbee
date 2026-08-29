# Download and extract Telink Zigbee SDK
set(DOWNLOAD_URL_SDK "http://wiki.telink-semi.cn/tools_and_sdk/Zigbee/Zigbee_SDK.zip")
set(SDK_EXPECTED_SHA256 "0d0859e2412e3a52e9c3ba012bbf5d5b7c30554b8a96632659bd500ea18f7aa3")
set(DOWNLOAD_PATH_SDK "${CMAKE_CURRENT_BINARY_DIR}/Zigbee_SDK.zip")
set(SDK_PREFIX_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/tl_zigbee_sdk")
set(SDK_PATCH_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/apply_sdk_patches.py")

if(NOT EXISTS ${DOWNLOAD_PATH_SDK})
  message(STATUS "Downloading SDK from ${DOWNLOAD_URL_SDK} to ${DOWNLOAD_PATH_SDK}")
  file(DOWNLOAD ${DOWNLOAD_URL_SDK} ${DOWNLOAD_PATH_SDK} SHOW_PROGRESS
    EXPECTED_HASH SHA256=${SDK_EXPECTED_SHA256})
endif()

file(SHA256 "${DOWNLOAD_PATH_SDK}" SDK_ARCHIVE_SHA256)
if(NOT SDK_ARCHIVE_SHA256 STREQUAL SDK_EXPECTED_SHA256)
  message(FATAL_ERROR
    "Pinned Telink SDK archive hash mismatch: expected ${SDK_EXPECTED_SHA256}, got ${SDK_ARCHIVE_SHA256}. "
    "Delete ${DOWNLOAD_PATH_SDK} and re-obtain the recorded archive.")
endif()

if(NOT EXISTS ${SDK_PREFIX_LOCATION})
  message(STATUS "Extracting SDK to ${SDK_PREFIX_LOCATION}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${DOWNLOAD_PATH_SDK} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# The extracted SDK is ignored; always verify/apply the tracked safety patch,
# even when this directory was extracted by an earlier invocation.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${SDK_PATCH_TOOL}" --sdk-root "${SDK_PREFIX_LOCATION}"
  RESULT_VARIABLE SDK_PATCH_RESULT
  OUTPUT_VARIABLE SDK_PATCH_OUTPUT
  ERROR_VARIABLE SDK_PATCH_ERROR)
if(NOT SDK_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Telink SDK patching failed:\n${SDK_PATCH_OUTPUT}${SDK_PATCH_ERROR}")
endif()
message(STATUS "${SDK_PATCH_OUTPUT}")

# Download and extract Telink toolchain
# set(DOWNLOAD_URL_TOOLCHAIN "https://www.dropbox.com/s/ths9rev0tvhhl96/Telink_IDE.zip?dl=1")
set(DOWNLOAD_URL_TOOLCHAIN "https://wiki.telink-semi.cn/tools_and_sdk/Tools/IDE/Telink_IDE.zip")
set(DOWNLOAD_PATH_TOOLCHAIN "${CMAKE_CURRENT_BINARY_DIR}/Telink_IDE.zip")
set(TOOLCHAIN_PREFIX_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/IDE")
set(TOOLCHAIN_INSTALL_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/app")
set(DOWNLOAD_URL_INNOEXTRACT "https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-windows.zip")
set(DOWNLOAD_PATH_INNOEXTRACT "${CMAKE_CURRENT_BINARY_DIR}/innoextract-1.9-windows.zip")
set(INNOEXTRACT_INSTALL_LOCATION "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT EXISTS ${DOWNLOAD_PATH_TOOLCHAIN})
  message(STATUS "Downloading SDK from ${DOWNLOAD_URL_TOOLCHAIN} to ${DOWNLOAD_PATH_TOOLCHAIN}")
  file(DOWNLOAD ${DOWNLOAD_URL_TOOLCHAIN} ${DOWNLOAD_PATH_TOOLCHAIN} SHOW_PROGRESS)
endif()

if(NOT EXISTS ${DOWNLOAD_PATH_INNOEXTRACT})
  message(STATUS "Downloading innoextract from ${DOWNLOAD_URL_INNOEXTRACT} to ${DOWNLOAD_PATH_INNOEXTRACT}")
  file(DOWNLOAD ${DOWNLOAD_URL_INNOEXTRACT} ${DOWNLOAD_PATH_INNOEXTRACT} SHOW_PROGRESS)
endif()

if(NOT EXISTS ${TOOLCHAIN_PREFIX_LOCATION})
  message(STATUS "Extracting SDK to ${TOOLCHAIN_PREFIX_LOCATION}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${DOWNLOAD_PATH_TOOLCHAIN} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(NOT EXISTS ${INNOEXTRACT_INSTALL_LOCATION}/innoextract.exe)
  message(STATUS "Extracting innoextract to ${INNOEXTRACT_INSTALL_LOCATION}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${DOWNLOAD_PATH_INNOEXTRACT} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
  
# Toolchain needs to be extracted first (is innosetup installer)
if(NOT EXISTS ${TOOLCHAIN_INSTALL_LOCATION})
  message(STATUS "Extracting toolchain to ${TOOLCHAIN_INSTALL_LOCATION}")
  execute_process(COMMAND ${INNOEXTRACT_INSTALL_LOCATION}/innoextract.exe ${TOOLCHAIN_PREFIX_LOCATION}/TelinkSDKv1.3.1.exe
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
