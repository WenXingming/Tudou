#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include "tudou/http/SslContext.h"

namespace {

constexpr char kValidCertPath[] = "/workspaces/Tudou/certs/test-cert.pem";
constexpr char kValidKeyPath[] = "/workspaces/Tudou/certs/test-key.pem";
constexpr char kMissingCertPath[] = "/workspaces/Tudou/certs/missing-cert.pem";

} // namespace

TEST(SslContextTest, InitWithValidCertificateCreatesServerSsl) {
    SslContext context;

    ASSERT_TRUE(context.init(kValidCertPath, kValidKeyPath));
    EXPECT_TRUE(context.is_initialized());

    SSL* ssl = context.create_ssl();
    ASSERT_NE(ssl, nullptr);
    SSL_free(ssl);
}

TEST(SslContextTest, FailedReinitializationClearsPreviousContext) {
    SslContext context;

    ASSERT_TRUE(context.init(kValidCertPath, kValidKeyPath));
    EXPECT_TRUE(context.is_initialized());

    EXPECT_FALSE(context.init(kMissingCertPath, kValidKeyPath));
    EXPECT_FALSE(context.is_initialized());
    EXPECT_EQ(context.create_ssl(), nullptr);
}