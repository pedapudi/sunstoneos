# Minimal static libcrypto from the pinned OpenSSL release, for station SAE.
# The source stays in the source cache (tools/fetch-sources.py openssl). The
# library is built once per build directory with the target C compiler and
# the freestanding C headers; checks/sae-openssl.py builds the same
# configuration for the host. It reads the two lists below, so keep them as
# plain whitespace-separated values.
set(SUNSTONE_OPENSSL_DIR "" CACHE PATH "Pinned OpenSSL source tree")
set(SUNSTONE_OPENSSL_JOBS 4 CACHE STRING "Parallel jobs for the OpenSSL library build")
# No operating-system seed source, threads, sockets, files or console; only
# SHA-1/SHA-2, HMAC, CMAC, AES, the prime-field EC curves and the BN
# arithmetic that WPA2 and SAE use remain, with the default provider.
# Without the fetch cache each fetch rebuilds its algorithm methods, which
# keeps about 65 KB less on the FreeRTOS heap for a slower hash setup.
set(SUNSTONE_OPENSSL_OPTIONS
  --with-rand-seed=none no-shared no-module no-dso no-engine no-threads no-asm
  no-sock no-dgram no-http no-ui-console no-stdio no-posix-io no-async
  no-autoload-config no-atexit no-filenames no-err no-apps no-tests no-docs
  no-ssl no-tls no-dtls no-quic no-srtp no-psk no-ct no-ocsp no-cms no-ts
  no-cmp no-srp no-rfc3779 no-comp no-legacy no-secure-memory no-rdrand
  no-padlockeng no-afalgeng no-capieng no-loadereng no-pinshared no-multiblock
  no-bf no-cast no-camellia no-aria no-des no-rc2 no-rc4 no-md4 no-mdc2
  no-rmd160 no-whirlpool no-idea no-seed no-sm2 no-sm3 no-sm4 no-gost
  no-blake2 no-siphash no-poly1305 no-chacha no-scrypt no-argon2 no-ocb no-siv
  no-dsa no-dh no-ecx no-ec2m no-ml-dsa no-ml-kem no-slh-dsa no-thread-pool
  no-default-thread-pool no-jitter no-sm2-precomp no-deprecated no-cached-fetch)
# The C library has no nanosleep declaration, syslog or directory listing.
set(SUNSTONE_OPENSSL_DEFINES -DOPENSSL_USE_USLEEP -DNO_SYSLOG)

# A Unix-like target with 64-bit BN words and the build's compiler. Write the
# Configure target and the dirent.h stub into <dir> and return the target name.
function(sunstone_openssl_target dir output)
  file(MAKE_DIRECTORY "${dir}/compat")
  file(CONFIGURE OUTPUT "${dir}/sunstone.conf" CONTENT [[
my %targets = (
    "sunstone-64" => {
        inherit_from => [ "BASE_unix" ],
        bn_ops => "SIXTY_FOUR_BIT_LONG",
        lib_cppflags => "-DL_ENDIAN",
    },
);
]])
  # Only OpenSSL's unused directory helper includes dirent.h. A reachable
  # use leaves these functions undefined and fails the link.
  file(CONFIGURE OUTPUT "${dir}/compat/dirent.h" CONTENT [[
typedef struct sunstone_openssl_dir DIR;
struct dirent { char d_name[256]; };
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
]])
  set(${output} sunstone-64 PARENT_SCOPE)
endfunction()

# Build libcrypto.a while configuring, so that every target which copies a
# radio target's sources and link options also finds the library and headers.
# The tree is rebuilt only when its identity changes. Sets the variables
# SUNSTONE_OPENSSL_LIBRARY and SUNSTONE_OPENSSL_INCLUDES.
function(sunstone_openssl_crypto)
  set(dir "${CMAKE_BINARY_DIR}/openssl")
  set(tree "${dir}/tree")
  set(SUNSTONE_OPENSSL_LIBRARY "${tree}/libcrypto.a" PARENT_SCOPE)
  set(SUNSTONE_OPENSSL_INCLUDES "${tree}/include" "${SUNSTONE_OPENSSL_DIR}/include" PARENT_SCOPE)
  get_property(ready GLOBAL PROPERTY SUNSTONE_OPENSSL_READY)
  if(ready)
    return()
  endif()
  if(NOT SUNSTONE_OPENSSL_DIR OR NOT SUNSTONE_NETWORK_C_HEADERS)
    message(FATAL_ERROR "SAE requires SUNSTONE_OPENSSL_DIR and SUNSTONE_NETWORK_C_HEADERS")
  endif()
  find_package(Perl REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(SUNSTONE_GNU_MAKE NAMES gmake make REQUIRED)
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${root}/checks/sae-openssl.py"
    --verify-source "${SUNSTONE_OPENSSL_DIR}" RESULT_VARIABLE verify OUTPUT_VARIABLE log ERROR_VARIABLE log)
  if(NOT verify EQUAL 0)
    message(FATAL_ERROR "OpenSSL source authentication failed: ${log}")
  endif()
  sunstone_openssl_target("${dir}" name)
  set(ar "${CMAKE_C_COMPILER_AR}")
  set(ranlib "${CMAKE_C_COMPILER_RANLIB}")
  if(NOT ar OR NOT ranlib)
    set(ar "${CMAKE_AR}")
    set(ranlib "${CMAKE_RANLIB}")
  endif()
  set(flags --target=${CMAKE_C_COMPILER_TARGET} -march=armv8-a -mstrict-align -ffreestanding
    -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -Os
    ${SUNSTONE_OPENSSL_DEFINES} -isystem "${dir}/compat" -isystem "${SUNSTONE_NETWORK_C_HEADERS}")
  sunstone_path_maps(maps)
  foreach(map IN LISTS maps)
    list(APPEND flags "-ffile-prefix-map=${map}")
  endforeach()
  list(JOIN flags " " cflags)
  # The archive hash, options, target and compiler identify the library.
  file(READ "${root}/sources.lock.json" lock)
  string(JSON archive_sha GET "${lock}" openssl sha256)
  string(JSON revision GET "${lock}" openssl revision)
  file(READ "${dir}/sunstone.conf" conf)
  sunstone_portable_text(portable "${cflags}")
  set(identity "openssl ${revision} ${archive_sha}\n${name}\n${conf}${SUNSTONE_OPENSSL_OPTIONS}\n${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n${portable}\n")
  set(built "")
  if(EXISTS "${tree}/libcrypto.a" AND EXISTS "${dir}/build-identity.txt")
    file(READ "${dir}/build-identity.txt" built)
  endif()
  if(NOT built STREQUAL identity)
    message(STATUS "Building the minimal OpenSSL libcrypto in ${tree}")
    file(REMOVE "${dir}/build-identity.txt")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}")
    execute_process(COMMAND "${PERL_EXECUTABLE}" "${SUNSTONE_OPENSSL_DIR}/Configure"
        "--config=${dir}/sunstone.conf" ${name} "CC=${CMAKE_C_COMPILER}" "AR=${ar}" "RANLIB=${ranlib}"
        "CFLAGS=${cflags}" ${SUNSTONE_OPENSSL_OPTIONS}
      WORKING_DIRECTORY "${tree}" RESULT_VARIABLE result OUTPUT_FILE "${dir}/configure.log" ERROR_FILE "${dir}/configure.log")
    if(result EQUAL 0)
      execute_process(COMMAND "${SUNSTONE_GNU_MAKE}" -j${SUNSTONE_OPENSSL_JOBS} build_generated libcrypto.a
        WORKING_DIRECTORY "${tree}" RESULT_VARIABLE result OUTPUT_FILE "${dir}/build.log" ERROR_FILE "${dir}/build.log")
    endif()
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "OpenSSL libcrypto build failed; see ${dir}/configure.log and build.log")
    endif()
    file(WRITE "${dir}/build-identity.txt" "${identity}")
  endif()
  set_property(GLOBAL PROPERTY SUNSTONE_OPENSSL_READY 1)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/sources.lock.json")
endfunction()

# Use OpenSSL for all RSN cryptography of a target that already called
# sunstone_enable_radio_security, and add station SAE. The internal hash, AES
# and HMAC files would duplicate crypto_openssl.c symbols, so they leave.
function(sunstone_enable_radio_sae target upstream)
  sunstone_openssl_crypto()
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  get_target_property(sources ${target} SOURCES)
  foreach(name IN ITEMS sha1.c sha1-internal.c sha256.c sha256-internal.c md5.c md5-internal.c
      aes-internal.c aes-internal-enc.c aes-internal-dec.c aes-wrap.c aes-unwrap.c aes-omac1.c)
    list(REMOVE_ITEM sources "${upstream}/src/crypto/${name}")
  endforeach()
  set_property(TARGET ${target} PROPERTY SOURCES ${sources})
  set(sae_sources crypto/crypto_openssl.c crypto/dh_groups.c crypto/sha256-kdf.c
    crypto/sha384-kdf.c crypto/sha512-kdf.c crypto/sha384-prf.c crypto/sha512-prf.c common/sae.c common/dragonfly.c)
  list(TRANSFORM sae_sources PREPEND "${upstream}/src/")
  # Native images have no C library; the last file supplies what OpenSSL uses.
  target_sources(${target} PRIVATE ${sae_sources} "${root}/drivers/net/aic8800_sae.c"
    "${root}/drivers/net/aic8800_openssl.c" "${root}/drivers/net/aic8800_openssl_libc.c")
  set_source_files_properties(${sae_sources} TARGET_DIRECTORY ${target}
    PROPERTIES COMPILE_OPTIONS "-include;${root}/include/aic8800_supplicant_port.h;-Wno-unused-parameter")
  target_compile_definitions(${target} PRIVATE SUNSTONE_AIC8800_SAE)
  target_include_directories(${target} PRIVATE ${SUNSTONE_OPENSSL_INCLUDES})
  # Link options and link dependencies, unlike link libraries, are what the
  # radio model copies (cmake/radio-dispatch.cmake) carry over.
  target_link_options(${target} PRIVATE "${SUNSTONE_OPENSSL_LIBRARY}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${SUNSTONE_OPENSSL_LIBRARY}")
endfunction()
