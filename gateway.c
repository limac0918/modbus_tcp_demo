/*
 * 基础网关：OPC-UA Server + Modbus Client
 *
 * 功能：
 *   - OPC-UA Server 监听 4840，暴露两个节点：
 *     ns=1;s=Temperature  (Int32, 只读)  ← 从 PLC 40001 轮询
 *     ns=1;s=SetSpeed     (Int32, 可写)  ← Client 写 → 写 PLC 40002
 *
 *   - 后台线程每 500ms 当 Modbus Client，读 PLC 保持寄存器
 *
 * 编译：
 *   gcc -std=c9x gateway.c -I/usr/local/include -L/usr/local/lib \
 *       -lopen62541 -lmodbus -lpthread -o gateway
 */

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <modbus.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* ─── 配置 ─── */
#define PLC_IP        "10.29.4.166"
#define PLC_PORT      502
#define PLC_SLAVE_ID  1
#define REG_TEMP      0       /* 40001 对应 Modbus 地址 0 */
#define REG_SPEED     1       /* 40002 对应 Modbus 地址 1 */
#define POLL_INTERVAL_MS  500

static UA_Server *server = NULL;
static modbus_t *mb = NULL;

/* ─── SetSpeed 的写回调：Client 写 OPC-UA 节点 → 写 PLC ─── */
static void
writeSetSpeed(UA_Server *srv, const UA_NodeId *sessionId,
              void *sessionContext, const UA_NodeId *nodeId,
              void *nodeContext, const UA_NumericRange *range,
              const UA_DataValue *newValue) {
    if (!newValue->hasValue || newValue->value.type != &UA_TYPES[UA_TYPES_INT32]) {
        fprintf(stderr, "[gateway] writeSetSpeed: bad type\n");
        return;
    }

    UA_Int32 speed = *(UA_Int32 *)newValue->value.data;
    printf("[gateway] OPC-UA write SetSpeed = %d → Modbus write REG %d\n",
           speed, REG_SPEED);

    /* 翻译成 Modbus 写单个保持寄存器 */
    uint16_t reg = (uint16_t)speed;
    int rc = modbus_write_register(mb, REG_SPEED, reg);
    if (rc == -1) {
        fprintf(stderr, "[gateway] Modbus write failed: %s\n", modbus_strerror(errno));
    }
}

/* ─── 后台线程：定时读 PLC → 更新 OPC-UA 节点（含自动重连）─── */
static void *
pollPlcThread(void *arg) {
    (void)arg;

    while (1) {
        uint16_t reg = 0;
        int rc = modbus_read_registers(mb, REG_TEMP, 1, &reg);

        if (rc == -1) {
            fprintf(stderr, "[gateway] Modbus read failed: %s, retrying...\n",
                    modbus_strerror(errno));

            /* 断线重连 */
            modbus_close(mb);
            usleep(1000 * 1000);  /* 等 1 秒再重连 */

            if (modbus_connect(mb) == -1) {
                fprintf(stderr, "[gateway] Reconnect failed: %s\n",
                        modbus_strerror(errno));
            } else {
                printf("[gateway] Reconnected to %s:%d\n", PLC_IP, PLC_PORT);
            }
            continue;
        }

        UA_Int32 temp = (UA_Int32)reg;
        printf("[gateway] Modbus read TEMP = %d → update OPC-UA node\n", temp);

        /* 更新 OPC-UA 节点值 */
        UA_Variant val;
        UA_Variant_init(&val);
        UA_Variant_setScalarCopy(&val, &temp, &UA_TYPES[UA_TYPES_INT32]);

        UA_NodeId tempNode = UA_NODEID_STRING(1, "Temperature");
        UA_Server_writeValue(server, tempNode, val);
        UA_Variant_clear(&val);

        usleep(POLL_INTERVAL_MS * 1000);
    }
    return NULL;
}

int main(void) {
    /* 1. 初始化 Modbus Client */
    mb = modbus_new_tcp(PLC_IP, PLC_PORT);
    if (!mb) {
        fprintf(stderr, "[gateway] modbus_new_tcp failed\n");
        return 1;
    }
    modbus_set_slave(mb, PLC_SLAVE_ID);
    if (modbus_connect(mb) == -1) {
        fprintf(stderr, "[gateway] Modbus connect failed: %s\n",
                modbus_strerror(errno));
        modbus_free(mb);
        return 1;
    }
    printf("[gateway] Modbus connected to %s:%d\n", PLC_IP, PLC_PORT);

    /* 2. 创建 OPC-UA Server */
    server = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server));

    /* 3. 加 Temperature 节点（只读，后台线程更新） */
    UA_VariableAttributes tempAttr = UA_VariableAttributes_default;
    UA_Int32 initTemp = 0;
    UA_Variant_setScalar(&tempAttr.value, &initTemp, &UA_TYPES[UA_TYPES_INT32]);
    tempAttr.displayName = UA_LOCALIZEDTEXT("en-US", "Temperature");
    tempAttr.description = UA_LOCALIZEDTEXT("en-US", "PLC register 40001");
    tempAttr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    tempAttr.accessLevel = UA_ACCESSLEVELMASK_READ;

    UA_NodeId tempNodeId = UA_NODEID_STRING(1, "Temperature");
    UA_QualifiedName tempName = UA_QUALIFIEDNAME(1, "Temperature");
    UA_Server_addVariableNode(server, tempNodeId,
        UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
        tempName, UA_NS0ID(BASEDATAVARIABLETYPE),
        tempAttr, NULL, NULL);

    /* 4. 加 SetSpeed 节点（可写，write callback 写 PLC） */
    UA_VariableAttributes speedAttr = UA_VariableAttributes_default;
    UA_Int32 initSpeed = 0;
    UA_Variant_setScalar(&speedAttr.value, &initSpeed, &UA_TYPES[UA_TYPES_INT32]);
    speedAttr.displayName = UA_LOCALIZEDTEXT("en-US", "SetSpeed");
    speedAttr.description = UA_LOCALIZEDTEXT("en-US", "Write to PLC register 40002");
    speedAttr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    speedAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    UA_NodeId speedNodeId = UA_NODEID_STRING(1, "SetSpeed");
    UA_QualifiedName speedName = UA_QUALIFIEDNAME(1, "SetSpeed");
    UA_Server_addVariableNode(server, speedNodeId,
        UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
        speedName, UA_NS0ID(BASEDATAVARIABLETYPE),
        speedAttr, NULL, NULL);

    /* 注册 write callback */
    UA_ValueCallback speedCb;
    speedCb.onRead = NULL;
    speedCb.onWrite = writeSetSpeed;
    UA_Server_setVariableNode_valueCallback(server, speedNodeId, speedCb);

    printf("[gateway] OPC-UA Server listening on opc.tcp://0.0.0.0:4840\n");

    /* 5. 启动后台轮询线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, pollPlcThread, NULL);

    /* 6. 跑 OPC-UA Server（阻塞） */
    UA_Server_runUntilInterrupt(server);

    /* 7. 清理 */
    pthread_cancel(tid);
    UA_Server_delete(server);
    modbus_close(mb);
    modbus_free(mb);
    return 0;
}
//（注：内容由AI生成）
