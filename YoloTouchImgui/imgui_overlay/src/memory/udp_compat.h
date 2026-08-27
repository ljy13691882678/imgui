// udp_actors.h 的轻量依赖适配：参考工程依赖 <engine.h>(Vector3) 与 "config.h"(g_Config)。
// 本项目不引入整套参考引擎头，这里提供最小等价类型/开关即可让 udp_actors.h 编译。
#pragma once

// 纯聚合(无默认成员初始化、无构造函数)，保持 C++17 aggregate 初始化语义：
// Vector3{0,0,0} / Vector3{} 都可用；字段直接读写。
struct Vector3 {
    float x, y, z;
};

struct Vector2 {
    float x, y;
};

// UDP 解密总开关，等价参考工程的 g_Config.EnableUdpDecrypt。
struct UdpDecryptConfig {
    bool EnableUdpDecrypt = false;
};
inline UdpDecryptConfig g_Config;