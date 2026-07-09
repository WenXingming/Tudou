#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Tudou TCP JSON-RPC 2.0 跨语言（Python）免 IDL/Stub 客户端示例
作者：wenxingming
项目：https://github.com/WenXingming/Tudou
"""

import socket
import json

def call_rpc(host, port, request_obj):
    # 1. 建立 TCP 套接字连接
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    
    # 2. 序列化为 JSON 字符串，尾部追加 \n 分包结束符
    payload = json.dumps(request_obj) + '\n'
    print(f"--> 发送请求: {payload.strip()}")
    s.sendall(payload.encode('utf-8'))
    
    # 3. 循环接收对端回包数据，直至读取到 \n
    response_data = b""
    while b'\n' not in response_data:
        chunk = s.recv(1024)
        if not chunk:
            break
        response_data += chunk
        
    s.close()
    
    # 4. 剥离换行符并反序列化输出
    response_str = response_data.decode('utf-8').strip()
    print(f"<-- 收到响应: {response_str}")
    return json.loads(response_str)

def main():
    host = "127.0.0.1"
    port = 8090
    
    print("=== 1. 测试常规单请求 (add) ===")
    req_add = {
        "jsonrpc": "2.0",
        "method": "add",
        "params": [45, 55],
        "id": 100
    }
    res_add = call_rpc(host, port, req_add)
    print(f"计算结果 (result): {res_add.get('result')}\n")
    
    print("=== 2. 测试常规单请求 (greet) ===")
    req_greet = {
        "jsonrpc": "2.0",
        "method": "greet",
        "params": ["Tudou Developer"],
        "id": 101
    }
    res_greet = call_rpc(host, port, req_greet)
    print(f"打招呼结果 (result): {res_greet.get('result')}\n")
    
    print("=== 3. 测试批量请求 (Batch Requests) ===")
    req_batch = [
        {"jsonrpc": "2.0", "method": "greet", "params": ["Python"], "id": 200},
        {"jsonrpc": "2.0", "method": "add", "params": [99, 1], "id": 201}
    ]
    res_batch = call_rpc(host, port, req_batch)
    print(f"批量聚合结果: {res_batch}\n")

if __name__ == "__main__":
    main()
