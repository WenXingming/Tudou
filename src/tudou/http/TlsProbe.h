// ============================================================================
// TlsProbe.h
// 探测当前编译与运行环境是否支持 Linux Kernel TLS (kTLS) 的底层能力。
// ============================================================================

#pragma once

class TlsProbe {
public:
    // 返回编译时和运行时是否都支持 Linux kTLS。
    static bool is_kernel_tls_supported();
};
