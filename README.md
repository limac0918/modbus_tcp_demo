# Modbus TCP ↔ OPC-UA 网关 Demo（modbus-opcua-gateway）

一个将工业 **Modbus TCP 设备（PLC）数据桥接到 OPC-UA** 的轻量级网关 Demo。
网关对外作为 **OPC-UA Server** 暴露数据节点，对内作为 **Modbus Client** 从 PLC 轮询/写入数据；
配套一个 **OPC-UA 订阅客户端**示例，演示订阅推送（数据变化自动通知）。

> 代码由 AI 生成，仅供学习参考，部署前请按实际环境核对。

---

## 一、架构与数据流

```
┌────────────┐   Modbus TCP    ┌──────────────────────────────┐   OPC-UA (订阅推送)   ┌─────────────┐
│    PLC     │ ───────────────► │           gateway            │ ────────────────────► │  myClient3  │
│ 10.29.4.166│   502 / slave=1  │  Modbus Client + OPC-UA Svr  │    opc.tcp://:4840    │  订阅客户端  │
│  :502      │                  │        监听 4840             │                       │             │
└────────────┘                  └──────────────────────────────┘                       └─────────────┘
      ▲                                   │
      │           读 40001（轮询 500ms）   │  Temperature → OPC-UA 节点（只读）
      │           写 40002（OPC-UA 写入）  │  SetSpeed    → OPC-UA 节点（可写）
```

**数据流说明：**

- **温度上行**：gateway 后台线程每 500ms 读 PLC 保持寄存器 40001 → 更新 OPC-UA 节点 `Temperature` → 订阅客户端自动收到变化通知；
- **速度下行**：OPC-UA Client 写节点 `SetSpeed` → gateway 写回调触发 Modbus 写寄存器 40002 → 写入 PLC。

## 二、OPC-UA 节点映射

| OPC-UA 节点 | 数据类型 | 权限 | Modbus 寄存器 | 数据方向 |
|---|---|---|---|---|
| `ns=1;s=Temperature` | Int32 | 只读 | 40001（地址 0） | PLC → 网关 → Client |
| `ns=1;s=SetSpeed` | Int32 | 可写 | 40002（地址 1） | Client → 网关 → PLC |

## 三、功能特性

- **双向桥接**：PLC 寄存器 ↔ OPC-UA 节点，读写双向打通；
- **定时轮询**：后台线程每 500ms 读一次保持寄存器（可配置）；
- **断线自动重连**：Modbus 读失败自动断开，1 秒后重连，日志可见；
- **写回调透传**：OPC-UA 写入自动翻译为 Modbus 写单寄存器；
- **订阅推送**：客户端订阅节点，数据变化 100ms 采样自动通知。

## 四、文件结构

| 文件 | 说明 |
|---|---|
| `gateway.c` | 网关主程序（OPC-UA Server + Modbus Client） |
| `myClient3.c` | OPC-UA 订阅客户端示例（订阅 Temperature 节点） |
| `build.sh` | 一键构建 Docker 镜像脚本（自动编译/收集依赖/构建） |
| `Dockerfile` | 容器化定义（Ubuntu 24.04 基础镜像） |
| `README.md` | 本文档 |

## 五、环境依赖

| 依赖 | 用途 | 安装（Ubuntu/Debian） |
|---|---|---|
| gcc + pkg-config | 编译 | `sudo apt install gcc pkg-config` |
| open62541 | OPC-UA 库（Server/Client） | `sudo apt install libopen62541-dev` |
| libmodbus | Modbus 库 | `sudo apt install libmodbus-dev` |
| Docker（可选） | 容器化部署 | `sudo apt install docker.io` |

> 编译依赖库需安装到系统默认路径（或调整 `-I`/`-L` 参数）。

## 六、编译

```bash
# ① 编译网关
gcc -std=c9x gateway.c -I/usr/local/include -L/usr/local/lib \
    -lopen62541 -lmodbus -lpthread -o gateway
# 或（若库已注册 pkg-config）：
gcc -std=c9x gateway.c $(pkg-config --cflags --libs open62541 libmodbus) -lpthread -o gateway

# ② 编译订阅客户端
gcc -std=c9x myClient3.c $(pkg-config --cflags --libs open62541) -lpthread -o myClient3
```

## 七、运行

```bash
# ① 启动网关（终端 1）
./gateway
# 预期日志：
#   [gateway] Modbus connected to 10.29.4.166:502
#   [gateway] OPC-UA Server listening on opc.tcp://0.0.0.0:4840

# ② 启动订阅客户端（终端 2，验证数据推送）
./myClient3 opc.tcp://127.0.0.1:4840
# 预期输出：
#   connected to opc.tcp://127.0.0.1:4840
#   subscription created, id=1
#   monitoring Temperature, waiting for notifications...
# 此时修改 PLC 40001 的值，终端自动打印：
#   [notify] Temperature = <值>
```

> 客户端默认运行 60 秒；可通过命令行参数指定其他 OPC-UA 端点：
> `./myClient3 opc.tcp://<服务器IP>:4840`

## 八、配置修改

网关连接参数在 `gateway.c` 顶部宏定义中：

| 宏 | 默认值 | 说明 |
|---|---|---|
| `PLC_IP` | `10.29.4.166` | PLC 地址（按实际环境修改） |
| `PLC_PORT` | `502` | Modbus TCP 端口 |
| `PLC_SLAVE_ID` | `1` | 从站 ID |
| `REG_TEMP` | `0` | 温度寄存器（40001 对应地址 0） |
| `REG_SPEED` | `1` | 速度寄存器（40002 对应地址 1） |
| `POLL_INTERVAL_MS` | `500` | 轮询间隔（毫秒） |

## 九、Docker 部署

```bash
# ① 一键构建（自动编译网关 → 收集动态库 → 构建镜像 → 清理）
./build.sh

# ② 运行（--network host 使容器直接访问 PLC 内网）
docker run --rm -it --network host --name gateway modbus-opcua-gateway
```

**build.sh 自动判断逻辑：**

| 条件 | 行为 |
|---|---|
| `gateway` 二进制不存在 | 重新编译 |
| `gateway.c` 比二进制新 | 重新编译 |
| `ldd gateway` 缺动态库 | 重新编译 |
| 都不满足 | 跳过编译，直接构建镜像 |

> - 镜像基于 `ubuntu:24.04`，运行入口为 `gateway`，暴露端口 4840；
> - 使用 `--network host` 便于访问内网 PLC；若需端口映射可改用
>   `docker run --rm -it -p 4840:4840 --name gateway modbus-opcua-gateway`（需保证容器可访问 PLC 网络）。

## 十、注意事项

1. `PLC_IP` 为内网示例地址，实际部署**必须修改**为真实 PLC 地址；
2. 网关与 PLC 需网络互通（同网段或路由可达）；
3. 代码由 AI 生成，接入生产环境前请充分测试；
4. 日志直接输出到终端，容器环境可通过 `docker logs` 查看。
