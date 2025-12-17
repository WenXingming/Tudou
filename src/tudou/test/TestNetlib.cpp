#include "TestNetlib.h"

#include <iostream>
#include <unistd.h>

#include "../tudou/Channel.h"
#include "../tudou/EpollPoller.h"
#include "../tudou/EventLoop.h"
#include <cstring>

TestNetlib::TestNetlib() {
    loop.reset(new EventLoop());

    int fd = 0; // fd = 0 标准输入
    channel.reset(new Channel(loop.get(), fd));
    channel->set_read_callback([](Channel& channel) {
        char buf[1024]{};
        int fd = channel.get_fd(); // 不要直接使用 fd，应通过 channel 获取
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << "Read " << n << " bytes from stdin: " << buf << std::endl;
        }
        else if (n == 0) {
            std::cout << "stdin closed\n";
            channel.disable_reading();
            // 如果你希望整个事件循环退出，可以加：loop->quit();
        }
        else {
            std::cout << "Read error, n=" << n << ", errno=" << errno << " (" << strerror(errno) << ")\n";
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 典型的非阻塞场景: 本轮数据读完了，什么都不做，等下一次 EPOLLIN
                return;
            }
            else {
                // 真正的错误，视情况关闭
                channel.disable_reading();
                // loop->quit();
            }
        }
        });
    channel->enable_reading();
}

TestNetlib::~TestNetlib() {
}

void TestNetlib::start() {
    loop->loop();
}
