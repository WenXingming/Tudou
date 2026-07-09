// ============================================================================
// TlsProbeTest.cpp
// ============================================================================

#include <gtest/gtest.h>
#include "tudou/http/TlsProbe.h"

#if defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
#endif

TEST(TlsProbeTest, DetectsPlatformSupport) {
#if !defined(__linux__)
    // On non-Linux platforms, kTLS should never be supported
    EXPECT_FALSE(TlsProbe::is_kernel_tls_supported());
#else
    // On Linux, TlsProbe output should match our manual runtime check
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool directSupport = false;
    if (fd >= 0) {
        int ret = ::setsockopt(fd, IPPROTO_TCP, 31, "tls", sizeof("tls"));
        int err = errno;
        ::close(fd);
        directSupport = (ret == 0 || err == ENOTCONN);
    }

#if __has_include(<linux/tls.h>)
    EXPECT_EQ(TlsProbe::is_kernel_tls_supported(), directSupport);
#else
    // If the header is missing at compile time, TlsProbe should be false
    EXPECT_FALSE(TlsProbe::is_kernel_tls_supported());
#endif

#endif
}
