#ifndef J1939_TYPES_HPP
#define J1939_TYPES_HPP

#include <cstdint>

namespace j1939sim
{

    namespace J1939Timeouts
    {
        constexpr uint32_t T1 = 750;  // Time between CTS messages received from the destination (ms)
        constexpr uint32_t T2 = 1250; // Time between RTS and first CTS received (ms)
        constexpr uint32_t T3 = 1250; // Time limit for receiver to send next CTS (ms)
        constexpr uint32_t T4 = 1050; // Time between last data packet and EndOfMsgAck (ms)
        constexpr uint32_t Tr = 200;  // Time between data packets received (ms)
        constexpr uint32_t Th = 500;  // Time to hold unused connection resources (ms)
    }

    // 会话状态
    enum class SessionState
    {
        WAIT_CTS, // 发送方等待CTS
        SENDING,  // 发送数据包
        WAIT_ACK,
        RECEIVING, // 接收数据包
        COMPLETE   // 会话完成
    };

    // 传输协议命令类型
    enum class TpCmType
    {
        RTS = 16,
        CTS = 17,
        EndOfMsgAck = 19,
        BAM = 32,
        Abort = 255
    };

    // 连接中止原因
    enum class AbortReason : uint8_t
    {
        // 由SAE J1939-21 规范定义的Abort原因
        NO_RESOURCES = 1,        // 无可用资源
        TIMEOUT = 2,             // 超时
        CTS_WHILE_DT = 3,        // 在接收数据包时收到CTS
        INCOMPLETE_TRANSFER = 4, // 未完成的传输
        BAD_SEQUENCE = 5,        // 序列号错误
        DUP_SEQUENCE = 6,        // 重复的序列号
        NODE_BUSY = 7,           // 节点忙
        CTS_LIMIT_EXCEEDED = 8,  // 超过了CTS请求的包数量
        CONFIG_ERROR = 9,        // 节点配置错误
        CONN_LOST = 10,          // 连接丢失
        ABORT_BY_RECEIVER = 11,  // 接收方主动中止
        ABORT_BY_SENDER = 12,    // 发送方主动中止
        BAD_PGN = 13,            // 不支持的PGN
        MSG_SIZE_ERROR = 14,     // 消息大小错误
        PROTOCOL_ERROR = 15,     // 协议错误
        RESOURCES_BUSY = 16,     // 资源忙
        RESPONSE_TIMEOUT = 17,   // 响应超时
        GENERAL_ERROR = 254,     // 一般错误
        SYSTEM_ERROR = 255       // 系统错误
    };

} // namespace j1939sim

#endif // J1939_TYPES_HPP
