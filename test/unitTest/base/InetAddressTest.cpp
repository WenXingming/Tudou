#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <stdexcept>

#include "base/InetAddress.h"

TEST(InetAddressTest, ConstructFromIpAndPortExposesNormalizedAccessors) {
    InetAddress address("127.0.0.1", 8080);

    EXPECT_EQ(address.get_ip(), "127.0.0.1");
    EXPECT_EQ(address.get_port(), 8080);
    EXPECT_EQ(address.get_ip_port(), "127.0.0.1:8080");
    EXPECT_EQ(address.get_sockaddr().sin_family, AF_INET);
    EXPECT_EQ(address.get_sockaddr().sin_port, htons(8080));
}

TEST(InetAddressTest, ConstructFromSockaddrCopiesExistingIpv4Contract) {
    sockaddr_in nativeAddress{};
    nativeAddress.sin_family = AF_INET;
    nativeAddress.sin_port = htons(65535);
    ASSERT_EQ(inet_pton(AF_INET, "192.168.1.100", &nativeAddress.sin_addr), 1);

    InetAddress address(nativeAddress);

    EXPECT_EQ(address.get_ip(), "192.168.1.100");
    EXPECT_EQ(address.get_port(), 65535);
    EXPECT_EQ(address.get_ip_port(), "192.168.1.100:65535");
}

TEST(InetAddressTest, CopyAssignmentPreservesIpv4ValueContract) {
    InetAddress source("10.0.0.8", 9527);
    InetAddress target("127.0.0.1", 80);

    target = source;

    EXPECT_EQ(target.get_ip(), "10.0.0.8");
    EXPECT_EQ(target.get_port(), 9527);
    EXPECT_EQ(target.get_ip_port(), "10.0.0.8:9527");
}

TEST(InetAddressTest, ConstructFromInvalidIpv4TextThrowsInvalidArgument) {
    EXPECT_THROW(InetAddress("not-an-ip", 9000), std::invalid_argument);
}

TEST(InetAddressTest, ConstructFromNonIpv4SockaddrThrowsInvalidArgument) {
    sockaddr_in nativeAddress{};
    nativeAddress.sin_family = AF_UNIX;

    EXPECT_THROW((void)InetAddress(nativeAddress), std::invalid_argument);
}
