#include <iostream>
#include <unistd.h>

#include "TestNetlib.h"
#include "../tudou/EventLoop.h"
#include "../tudou/Channel.h"

TestNetlib::TestNetlib() {
    loop.reset(new EventLoop());

    channel.reset(new Channel(loop.get(), 0, 0, 0, nullptr, nullptr, nullptr, nullptr)); // fd = 0 (标准输入)
    channel->enable_reading();
    channel->set_read_callback([&](/* Timestamp receivetime */) {
        char buf[1024]{};
        ssize_t n = read(0, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::cout << "stdin: " << buf << std::endl;
        }
        });
    loop->update_channel(channel.get());
}

TestNetlib::~TestNetlib() {

}

void TestNetlib::start() {
    loop->loop();
}
