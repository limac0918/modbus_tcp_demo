/*
 * 订阅 Client：订阅 Temperature 节点，自动接收变化通知
 *
 * 编译：
 *   gcc -std=c9x myClient3.c $(pkg-config --cflags --libs open62541) -lpthread -o myClient3
 *
 * 运行：
 *   ./myClient3 opc.tcp://127.0.0.1:4840
 */

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_subscriptions.h>
#include <stdio.h>
#include <unistd.h>

/* 订阅通知回调：节点值变了会被调用 */
static void
dataChange(UA_Client *client, UA_UInt32 subId, void *subContext,
           UA_UInt32 monId, void *monContext, UA_DataValue *value) {
    if (value->hasValue && UA_Variant_isScalar(&value->value)) {
        UA_Int32 temp = *(UA_Int32 *)value->value.data;
        printf("[notify] Temperature = %d\n", temp);
    }
}

int main(int argc, char **argv) {
    const char *endpoint = "opc.tcp://127.0.0.1:4840";
    if (argc > 1) endpoint = argv[1];

    /* 1. 连接 */
    UA_Client *client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));
    UA_StatusCode rc = UA_Client_connect(client, endpoint);
    if (rc != UA_STATUSCODE_GOOD) {
        printf("connect failed: %s\n", UA_StatusCode_name(rc));
        UA_Client_delete(client);
        return 1;
    }
    printf("connected to %s\n", endpoint);

    /* 2. 创建订阅（100ms 推送一次） */
    UA_CreateSubscriptionRequest subReq = UA_CreateSubscriptionRequest_default();
    UA_CreateSubscriptionResponse subResp = UA_Client_Subscriptions_create(
        client, subReq, NULL, NULL, NULL);
    if (subResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        printf("create subscription failed\n");
        UA_Client_delete(client);
        return 1;
    }
    printf("subscription created, id=%u\n", subResp.subscriptionId);

    /* 3. 订阅 Temperature 节点 */
    UA_MonitoredItemCreateRequest monReq = UA_MonitoredItemCreateRequest_default(
        UA_NODEID_STRING(1, "Temperature"));
    monReq.requestedParameters.samplingInterval = 100;  /* 100ms 采样 */
    monReq.requestedParameters.queueSize = 10;
    monReq.requestedParameters.discardOldest = true;

    UA_MonitoredItemCreateResult monResp = UA_Client_MonitoredItems_createDataChange(
        client, subResp.subscriptionId,
        UA_TIMESTAMPSTORETURN_BOTH, monReq, NULL, dataChange, NULL);

    if (monResp.statusCode != UA_STATUSCODE_GOOD) {
        printf("create monitored item failed: %s\n", UA_StatusCode_name(monResp.statusCode));
    } else {
        printf("monitoring Temperature, waiting for notifications...\n");
        printf("(改 Slave 寄存器 0 的值，这里会自动打印)\n");
    }

    /* 4. 事件循环，跑 60 秒 */
    for (int i = 0; i < 600; i++) {
        UA_Client_run_iterate(client, 100);
    }

    /* 5. 清理 */
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return 0;
}
//（注：内容由AI生成）
