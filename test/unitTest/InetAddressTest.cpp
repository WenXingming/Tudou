#include <gtest/gtest.h>
#include "base/InetAddress.h"

// InetAddress 构造函数与基本访问接口测试
TEST(InetAddressTest, ConstructAndAccessors) {
    InetAddress addr("127.0.0.1", 8080);

    EXPECT_EQ(addr.get_ip(), "127.0.0.1");
    EXPECT_EQ(addr.get_port(), 8080);
    EXPECT_EQ(addr.get_ip_port(), "127.0.0.1:8080");
}

// 可以根据需要拓展更多边界情况测试
// 例如测试不同的 IP、端口组合
TEST(InetAddressTest, DifferentIpAndPort) {
    InetAddress addr("192.168.1.100", 65535);

    EXPECT_EQ(addr.get_ip(), "192.168.1.100");
    EXPECT_EQ(addr.get_port(), 65535);
    EXPECT_EQ(addr.get_ip_port(), "192.168.1.100:65535");
}
