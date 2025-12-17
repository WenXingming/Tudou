#pragma once


class TestHttpParser {
public:
    // 运行全部测试用例，0 表示通过，非 0 表示失败
    int run_all();

private:
    // 单个用例：解析一个简单 GET 请求
    int test_simple_get();
};

