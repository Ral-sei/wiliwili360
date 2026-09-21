cmake_minimum_required(VERSION 3.15)

# add cpr
if (PLATFORM_XBOX360)
    option(XBOX360_BUILD_CPR "Build CPR/libcurl while porting Xbox 360 TLS" OFF)
else ()
    set(XBOX360_BUILD_CPR ON)
endif ()
if (PLATFORM_XBOX360 AND NOT XBOX360_BUILD_CPR)
    # The TLS-only bring-up target must not configure host-oriented curl
    # feature tests. The real HTTP transport will replace this placeholder.
    add_library(cpr::cpr INTERFACE IMPORTED)
elseif (USE_SYSTEM_CPR)
    find_package(cpr CONFIG REQUIRED)
    if (cpr_FOUND)
        message(STATUS "Found cpr: ${cpr_CONFIG} (found version ${cpr_VERSION})")
    endif ()
else ()
    if (USE_SYSTEM_CURL)
        set(CPR_USE_SYSTEM_CURL ON)
    else ()
        set(CPR_USE_SYSTEM_CURL OFF)
    endif ()

    set(BUILD_SHARED_LIBS OFF)
    if (PLATFORM_XBOX360)
        option(XBOX360_ENABLE_SSL "Enable CPR SSL backend for Xbox 360" OFF)
        set(CPR_ENABLE_SSL ${XBOX360_ENABLE_SSL})
    else ()
        set(CPR_ENABLE_SSL ON)
    endif ()
    if (PLATFORM_DESKTOP)
        if (WIN32)
            set(CPR_FORCE_WINSSL_BACKEND ON)
        endif ()
    elseif (PLATFORM_IOS)
        set(CPR_USE_SYSTEM_CURL OFF)
        set(CPR_SKIP_CA_BUNDLE_SEARCH ON)
    elseif (PLATFORM_SWITCH OR PLATFORM_PS4 OR PLATFORM_PSV)
        set(CPR_USE_SYSTEM_CURL ON)
        set(CPR_FORCE_MBEDTLS_BACKEND ON)
    endif ()
    add_subdirectory(cpr EXCLUDE_FROM_ALL)
endif ()
if (PLATFORM_PS4)
    list(APPEND APP_PLATFORM_LIB SceNet mbedcrypto mbedtls mbedx509)
    set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
endif ()

# Xbox TLS slice from bili360-main. This is deliberately a separate target:
# CPR/libcurl integration is a later step, while this keeps the Xenon mbedTLS
# sources buildable and auditable on their own.
if (PLATFORM_XBOX360)
    option(XBOX360_BUILD_MBEDTLS "Build the bili360 Xbox mbedTLS static library" ON)
endif ()
if (PLATFORM_XBOX360 AND XBOX360_BUILD_MBEDTLS)
    if (NOT DEFINED XBOX360_BILI360_DIR)
        if (DEFINED ENV{BILI360_DIR})
            set(XBOX360_BILI360_DIR "$ENV{BILI360_DIR}")
        else ()
            message(FATAL_ERROR "XBOX360_BUILD_MBEDTLS requires XBOX360_BILI360_DIR or BILI360_DIR")
        endif ()
    endif ()
    set(XBOX360_MBEDTLS_DIR "${XBOX360_BILI360_DIR}/third_party/mbedtls")
    if (NOT EXISTS "${XBOX360_MBEDTLS_DIR}/include/mbedtls/config-xbox360.h")
        message(FATAL_ERROR "bili360-main mbedTLS tree not found under ${XBOX360_MBEDTLS_DIR}")
    endif ()
    set(XBOX360_MBEDTLS_SOURCES
        aes.c asn1parse.c asn1write.c base64.c bignum.c cipher.c cipher_wrap.c
        constant_time.c ctr_drbg.c ecdh.c ecdsa.c ecp.c ecp_curves.c entropy.c
        entropy_poll.c entropy_poll_xbox360.c error.c gcm.c md.c md5.c oid.c pem.c
        pk.c pk_wrap.c pkparse.c platform.c platform_gmtime_xbox360.c platform_util.c
        rsa.c rsa_internal.c sha1.c sha256.c sha512.c ssl_cache.c ssl_ciphersuites.c
        ssl_cli.c ssl_cookie.c ssl_msg.c ssl_ticket.c ssl_tls.c version.c
        version_features.c x509.c x509_crl.c x509_crt.c
    )
    list(TRANSFORM XBOX360_MBEDTLS_SOURCES PREPEND "${XBOX360_MBEDTLS_DIR}/library/")
    foreach (XBOX360_MBEDTLS_SOURCE IN LISTS XBOX360_MBEDTLS_SOURCES)
        if (NOT EXISTS "${XBOX360_MBEDTLS_SOURCE}")
            message(FATAL_ERROR "Missing bili360 mbedTLS source: ${XBOX360_MBEDTLS_SOURCE}")
        endif ()
    endforeach ()
    add_library(xbox360_mbedtls STATIC ${XBOX360_MBEDTLS_SOURCES})
    target_include_directories(xbox360_mbedtls PUBLIC
        "${XBOX360_MBEDTLS_DIR}/include"
    )
    target_compile_definitions(xbox360_mbedtls PUBLIC
        _XBOX
        # Xenon clang provides the 32-bit size_t builtin as unsigned long.
        # The legacy XDK CRT otherwise redeclares it as unsigned int.
        _SIZE_T_DEFINED
        # The XDK CRT exposes Microsoft's _snprintf spelling, not POSIX
        # snprintf. Point mbedTLS at the ABI-compatible XDK declaration.
        MBEDTLS_PLATFORM_STD_SNPRINTF=_snprintf
        MBEDTLS_CONFIG_FILE=\"mbedtls/config-xbox360.h\"
    )
    target_compile_options(xbox360_mbedtls PRIVATE -w)
    list(APPEND APP_PLATFORM_LIB xbox360_mbedtls)
    list(APPEND APP_PLATFORM_INCLUDE "${XBOX360_MBEDTLS_DIR}/include")
    set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
    set(APP_PLATFORM_INCLUDE ${APP_PLATFORM_INCLUDE} PARENT_SCOPE)
endif ()

if (PLATFORM_XBOX360)
    option(XBOX360_BUILD_CURL "Build the bili360 Xbox libcurl static library" OFF)
endif ()
if (PLATFORM_XBOX360 AND XBOX360_BUILD_CURL)
    if (NOT DEFINED XBOX360_BILI360_DIR)
        if (DEFINED ENV{BILI360_DIR})
            set(XBOX360_BILI360_DIR "$ENV{BILI360_DIR}")
        else ()
            message(FATAL_ERROR "XBOX360_BUILD_CURL requires XBOX360_BILI360_DIR or BILI360_DIR")
        endif ()
    endif ()
    set(XBOX360_CURL_DIR "${XBOX360_BILI360_DIR}/third_party/curl")
    if (NOT EXISTS "${XBOX360_CURL_DIR}/lib/curl_setup.h")
        message(FATAL_ERROR "bili360-main Xbox curl tree not found under ${XBOX360_CURL_DIR}")
    endif ()
    set(XBOX360_CURL_SOURCES
        base64.c bundles.c conncache.c connect.c content_encoding.c cookie.c
        curl_addrinfo.c curl_fnmatch.c curl_gethostname.c curl_memrchr.c
        curl_multibyte.c curl_sasl.c curl_threads.c dotdot.c easy.c escape.c
        fileinfo.c formdata.c getenv.c getinfo.c hash.c hmac.c hostasyn.c
        hostcheck.c hostip.c hostip4.c hostip6.c hostsyn.c if2ip.c inet_ntop.c
        inet_pton.c llist.c md5.c mprintf.c multi.c netrc.c non-ascii.c
        nonblock.c parsedate.c pipeline.c progress.c rawstr.c select.c sendf.c
        share.c slist.c socks.c speedcheck.c splay.c strdup.c strequal.c
        strerror.c strtok.c strtoofft.c timeval.c transfer.c url.c version.c
        warnless.c wildcard.c http.c http_chunks.c http_digest.c
        http_negotiate.c http_proxy.c asyn-thread.c vtls/vtls.c vtls/mbedtls.c
        xbox360_stubs.c
    )
    list(TRANSFORM XBOX360_CURL_SOURCES PREPEND "${XBOX360_CURL_DIR}/lib/")
    foreach (XBOX360_CURL_SOURCE IN LISTS XBOX360_CURL_SOURCES)
        if (NOT EXISTS "${XBOX360_CURL_SOURCE}")
            message(FATAL_ERROR "Missing bili360 Xbox curl source: ${XBOX360_CURL_SOURCE}")
        endif ()
    endforeach ()
    add_library(xbox360_curl STATIC ${XBOX360_CURL_SOURCES})
    target_include_directories(xbox360_curl PUBLIC
        "${XBOX360_CURL_DIR}/include"
        "${XBOX360_CURL_DIR}/lib"
        "${XBOX360_BILI360_DIR}/third_party/mbedtls/include"
    )
    target_compile_definitions(xbox360_curl PRIVATE
        BUILDING_LIBCURL CURL_STATICLIB _XBOX
        _MSC_VER=1600 _INTEGRAL_MAX_BITS=64
    )
    # The XDK also ships a share.h. Put curl's private headers first so
    # vtls.c resolves curl/lib/share.h rather than the unrelated XDK header.
    target_compile_options(xbox360_curl PRIVATE -w "-iquote${XBOX360_CURL_DIR}/lib")
    if (TARGET xbox360_mbedtls)
        target_link_libraries(xbox360_curl PUBLIC xbox360_mbedtls)
    else ()
        message(FATAL_ERROR "XBOX360_BUILD_CURL requires XBOX360_BUILD_MBEDTLS=ON")
    endif ()
    list(APPEND APP_PLATFORM_LIB xbox360_curl)
    list(APPEND APP_PLATFORM_INCLUDE "${XBOX360_CURL_DIR}/include")
    set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
    set(APP_PLATFORM_INCLUDE ${APP_PLATFORM_INCLUDE} PARENT_SCOPE)
endif ()

if (PLATFORM_XBOX360)
    set(XBOX360_ZLIB_DIR "$ENV{OXDK_DIR}/xbox/third-party/libSDLx/zlib")
    if (NOT EXISTS "${XBOX360_ZLIB_DIR}/zlib.h")
        message(FATAL_ERROR "OXDK zlib source not found under ${XBOX360_ZLIB_DIR}")
    endif ()
    add_library(xbox360_zlib STATIC
        ${XBOX360_ZLIB_DIR}/adler32.c
        ${XBOX360_ZLIB_DIR}/compress.c
        ${XBOX360_ZLIB_DIR}/crc32.c
        ${XBOX360_ZLIB_DIR}/deflate.c
        ${XBOX360_ZLIB_DIR}/gzio.c
        ${XBOX360_ZLIB_DIR}/infblock.c
        ${XBOX360_ZLIB_DIR}/infcodes.c
        ${XBOX360_ZLIB_DIR}/inffast.c
        ${XBOX360_ZLIB_DIR}/inflate.c
        ${XBOX360_ZLIB_DIR}/inftrees.c
        ${XBOX360_ZLIB_DIR}/infutil.c
        ${XBOX360_ZLIB_DIR}/trees.c
        ${XBOX360_ZLIB_DIR}/uncompr.c
        ${XBOX360_ZLIB_DIR}/zutil.c)
    target_include_directories(xbox360_zlib PUBLIC "${XBOX360_ZLIB_DIR}")
    target_compile_options(xbox360_zlib PRIVATE -w)
endif ()

# add libopencc
if (NOT USE_LIBROMFS AND NOT DISABLE_OPENCC AND NOT PLATFORM_IOS)
    set(BRLS_USE_OPENCC ON)
    if (USE_SYSTEM_OPENCC)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(opencc REQUIRED IMPORTED_TARGET opencc)
        add_library(libopencc ALIAS PkgConfig::opencc)
        set(BRLS_OPENCC_INCLUDE ${opencc_INCLUDE_DIRS})
    else ()
        set(BUILD_SHARED_LIBS OFF)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        add_subdirectory(OpenCC EXCLUDE_FROM_ALL)
        target_compile_options(libopencc PRIVATE -w)
        set(BRLS_OPENCC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/OpenCC/src)
    endif ()
else ()
    set(BRLS_USE_OPENCC OFF)
endif ()

# add QR-Code-generator
if (USE_SYSTEM_QRCODEGEN)
    find_package(qrcodegencpp REQUIRED GLOBAL)
    add_library(qrcode ALIAS qrcodegencpp::qrcodegencpp)
else ()
    add_library(qrcode STATIC ${CMAKE_CURRENT_SOURCE_DIR}/QR-Code-generator/cpp/qrcodegen.cpp)
    target_include_directories(qrcode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/QR-Code-generator/cpp)
    set_property(TARGET qrcode PROPERTY CXX_STANDARD 17)
endif ()

# add pystring
if (USE_SYSTEM_PYSTRING)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
        pkg_check_modules(pystring IMPORTED_TARGET pystring)
    endif ()
    if (pystring_FOUND)
        add_library(pystring ALIAS PkgConfig::pystring)
    else ()
        find_path(PYSTRING_INCLUDE_DIR pystring.h PATH_SUFFIXES pystring REQUIRED)
        find_library(PYSTRING_LIBRARY NAMES pystring REQUIRED)
        list(APPEND APP_PLATFORM_INCLUDE ${PYSTRING_INCLUDE_DIR})
        list(APPEND APP_PLATFORM_LIB ${PYSTRING_LIBRARY})
        set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
        set(APP_PLATFORM_INCLUDE ${APP_PLATFORM_INCLUDE} PARENT_SCOPE)
    endif ()
else ()
    add_library(pystring STATIC ${CMAKE_CURRENT_SOURCE_DIR}/pystring/pystring.cpp)
    target_include_directories(pystring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pystring)
    set_property(TARGET pystring PROPERTY CXX_STANDARD 17)
endif ()

# add lunasvg
if (USE_SYSTEM_LUNASVG)
    find_package(lunasvg REQUIRED GLOBAL)
    add_library(lunasvg ALIAS lunasvg::lunasvg)
else ()
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    add_subdirectory(lunasvg EXCLUDE_FROM_ALL)
endif ()

# add mongoose
if (USE_SYSTEM_MONGOOSE)
    find_library(MONGOOSE_LIBRARY NAMES mongoose REQUIRED)
    add_library(mongoose UNKNOWN IMPORTED)
    set_property(TARGET mongoose PROPERTY IMPORTED_LOCATION ${MONGOOSE_LIBRARY})
else ()
    set(MG_ENABLE_IPV6 ON)
    add_subdirectory(mongoose EXCLUDE_FROM_ALL)
endif ()

# add MemoryModule
if (MPV_BUNDLE_DLL)
    add_library (MemoryModule STATIC ${CMAKE_CURRENT_SOURCE_DIR}/MemoryModule/MemoryModule.c)
    target_include_directories(MemoryModule PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/MemoryModule)
endif ()

# add borealis
add_subdirectory(borealis/library)

# add libpdr (不使用 pdr 项目内的 cmake 配置，因为其内的 mongoose 不支持 psv/switch)
file(GLOB_RECURSE PDR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/libpdr/src/*.cpp)
add_library(pdr ${PDR_SRC})
set_property(TARGET pdr PROPERTY CXX_STANDARD 17)
target_include_directories(pdr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libpdr/include)
target_link_libraries(pdr mongoose tinyxml2)

add_library(wiliwiliLibExtra INTERFACE)
target_link_libraries(wiliwiliLibExtra INTERFACE cpr::cpr qrcode)
