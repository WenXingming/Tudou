/**
 * @file main.cpp
 * @brief Tudou 集成测试入口
 *
 * 当前集成测试：
 *  - 启动底层网络库（EventLoop + Channel），从 stdin 读取数据并打印到 stdout，
 *    用于验证 EpollPoller / Channel / EventLoop 的协同工作是否正常。
 *
 * 使用方式：
 *  1. 在项目根目录编译： cmake --build build
 *  2. 运行： ./build/src/tudou/integrationTest/integrationTest
 *  3. 在终端输入任意内容回车，观察输出；Ctrl+D 结束 stdin。
 */

#include "TestNetlib.h"

int main() {
	TestNetlib test;
	test.start();
	return 0;
}

