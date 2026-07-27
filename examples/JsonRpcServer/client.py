#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Tudou TCP JSON-RPC 2.0 跨语言 Python 第三方库 (jsonrpcclient) 调用示例
作者：wenxingming
项目：https://github.com/WenXingming/Tudou
"""

import socket
import json
from jsonrpcclient import request, parse, Ok, Error

def call_rpc(sock, method, params=None):
    # 1. 使用第三方库 jsonrpcclient 构建 JSON-RPC 2.0 标准请求对象
    req_obj = request(method, params) if params is not None else request(method)
    
    # 2. 序列化为单行文本，按 Tudou TCP 分帧规范追加 \n 结束符发送
    payload = json.dumps(req_obj) + '\n'
    print(f"--> [jsonrpcclient 构造发送]: {payload.strip()}")
    sock.sendall(payload.encode('utf-8'))

    # 3. 阻塞接收 C++ 服务端返回的 \n 单行响应文本
    response_data = b""
    while b'\n' not in response_data:
        chunk = sock.recv(1024)
        if not chunk:
            break
        response_data += chunk

    response_str = response_data.decode('utf-8').strip()
    print(f"<-- [C++ 服务端原样回包]: {response_str}")

    # 4. 使用第三方库 jsonrpcclient.parse 解析响应
    parsed = parse(json.loads(response_str))
    if isinstance(parsed, Ok):
        return parsed.result
    elif isinstance(parsed, Error):
        raise RuntimeError(f"RPC Error [{parsed.code}]: {parsed.message}")
    else:
        raise RuntimeError("Unknown RPC response type")

def main():
    host = "127.0.0.1"
    port = 8090

    # 建立 TCP Socket 连接
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))

    try:
        print("=== 1. 使用 jsonrpcclient 调用 C++ 服务端 add 方法 ===")
        res_add = call_rpc(sock, "add", [45, 55])
        print(f"第三方库解析计算结果: {res_add}\n")

        print("=== 2. 使用 jsonrpcclient 调用 C++ 服务端 greet 方法 ===")
        res_greet = call_rpc(sock, "greet", ["Tudou Python Client"])
        print(f"第三方库解析打招呼结果: {res_greet}\n")

    finally:
        sock.close()

if __name__ == "__main__":
    main()
