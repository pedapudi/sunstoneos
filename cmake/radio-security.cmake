# The caller owns the existing SDIO/runtime/Wi-Fi/data target and FreeRTOS port.
# External source remains outside Git; verify every source/header before use.
function(sunstone_enable_radio_security target upstream)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  get_filename_component(radio_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${radio_root}/checks/aic8800-security.py"
    --upstream "${upstream}" RESULT_VARIABLE radio_verify OUTPUT_VARIABLE radio_verify_log
    ERROR_VARIABLE radio_verify_error)
  if(NOT radio_verify EQUAL 0)
    message(FATAL_ERROR "RSN source authentication failed: ${radio_verify_log}${radio_verify_error}")
  endif()
  set(radio_rsn_sources
    rsn_supp/wpa.c rsn_supp/wpa_ie.c rsn_supp/pmksa_cache.c
    common/wpa_common.c common/ieee802_11_common.c utils/common.c utils/wpabuf.c
    crypto/sha1.c crypto/sha1-internal.c crypto/sha1-prf.c
    crypto/sha256.c crypto/sha256-internal.c crypto/sha256-prf.c
    crypto/md5.c crypto/md5-internal.c crypto/aes-internal.c
    crypto/aes-internal-enc.c crypto/aes-internal-dec.c
    crypto/aes-wrap.c crypto/aes-unwrap.c crypto/aes-omac1.c)
  list(TRANSFORM radio_rsn_sources PREPEND "${upstream}/src/")
  target_sources(${target} PRIVATE ${radio_rsn_sources}
    "${radio_root}/drivers/net/aic8800_security.c"
    "${radio_root}/drivers/net/aic8800_supplicant.c"
    "${radio_root}/drivers/net/aic8800_supplicant_os.c")
  set_source_files_properties(${radio_rsn_sources} TARGET_DIRECTORY ${target}
    PROPERTIES COMPILE_OPTIONS "-include;${radio_root}/include/aic8800_supplicant_port.h;-Wno-unused-parameter")
  target_compile_definitions(${target} PRIVATE SUNSTONE_AIC8800_RSN)
  target_include_directories(${target} PRIVATE "${radio_root}/include" "${upstream}/src" "${upstream}/src/utils")
  target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(${target} PRIVATE -Wl,--gc-sections)
endfunction()
