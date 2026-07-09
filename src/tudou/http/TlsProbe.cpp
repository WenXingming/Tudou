// ============================================================================
// TlsProbe.cpp
// ============================================================================

#include "tudou/http/TlsProbe.h"
#include <mutex>
#include <openssl/ssl.h>
#include <openssl/bio.h>

#if defined(__linux__) && defined(SSL_OP_ENABLE_KTLS) && defined(BIO_get_ktls_send)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>

#if __has_include(<linux/tls.h>)
#include <linux/tls.h>
#define TUDOU_HAS_LINUX_TLS_HDR 1
#else
#define TUDOU_HAS_LINUX_TLS_HDR 0
#endif

// Compatibility definitions for systems with older headers but newer kernels
#ifndef SOL_TLS
#define SOL_TLS 282
#endif

#ifndef TCP_ULP
#define TCP_ULP 31
#endif

namespace {

bool probe_kernel_tls_runtime() {
#if TUDOU_HAS_LINUX_TLS_HDR
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    // Attempt to register upper-layer protocol "tls".
    // A return of 0 (if somehow connected) or ENOTCONN (since it's an unconnected socket)
    // proves the kTLS kernel module is loaded and ULP is available.
    int ret = ::setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls"));
    int err = errno;
    ::close(fd);

    if (ret == 0 || err == ENOTCONN) {
        return true;
    }
    return false;
#else
    return false;
#endif
}

} // namespace

#endif

bool TlsProbe::is_kernel_tls_supported() {
#if defined(__linux__) && defined(SSL_OP_ENABLE_KTLS) && defined(BIO_get_ktls_send) && TUDOU_HAS_LINUX_TLS_HDR
    static bool supported = false;
    static std::once_flag probe_flag;
    std::call_once(probe_flag, []() {
        supported = probe_kernel_tls_runtime();
    });
    return supported;
#else
    return false;
#endif
}
