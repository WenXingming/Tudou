#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <string>

#include "tudou/http/SslContext.h"

namespace {

std::string cert_path(const char* fileName) {
    return std::string(TUDOU_SOURCE_DIR) + "/certs/" + fileName;
}

} // namespace

TEST(SslContextTest, InitWithValidCertificateCreatesServerSsl) {
    SslContext context;

    ASSERT_TRUE(context.init(cert_path("test-cert.pem"), cert_path("test-key.pem")));
    EXPECT_TRUE(context.is_initialized());

    SSL* ssl = context.create_ssl();
    ASSERT_NE(ssl, nullptr);
    SSL_free(ssl);
}

TEST(SslContextTest, FailedReinitializationClearsPreviousContext) {
    SslContext context;

    ASSERT_TRUE(context.init(cert_path("test-cert.pem"), cert_path("test-key.pem")));
    EXPECT_TRUE(context.is_initialized());

    EXPECT_FALSE(context.init(cert_path("missing-cert.pem"), cert_path("test-key.pem")));
    EXPECT_FALSE(context.is_initialized());
    EXPECT_EQ(context.create_ssl(), nullptr);
}