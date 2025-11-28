#include "TestNetlib.h"

#include <iostream>
#include <unistd.h>

#include "../tudou/Channel.h"
#include "../tudou/EpollPoller.h"
#include "../tudou/EventLoop.h"

TestNetlib::TestNetlib() {
    loop.reset(new EventLoop());

    channel.reset(new Channel(loop.get(), 0)); // fd = 0 (标准输入)
    channel->enable_reading();
    channel->set_read_callback([&](/* Timestamp receivetime */) {
        char buf[1024]{};
        ssize_t n = read(0, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::cout << "stdin: " << buf << std::endl;
        }
        });
    channel->update_to_register(); // 创建 channel 后，注册到 poller。
}

TestNetlib::~TestNetlib() {
}

void TestNetlib::start() {
    loop->loop();
}
