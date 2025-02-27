#ifndef J1939_TYPES_HPP
#define J1939_TYPES_HPP

#include <cstdint>

namespace j1939sim
{

    // 会话状态
    enum class SessionState
    {
        INIT,
        WAIT_CTS,
        SENDING,
        RECEIVING, // 添加接收状态
        WAIT_ACK,
        COMPLETE,
        ERROR
    };

    // 传输协议命令类型
    enum class TpCmType
    {
        RTS = 16,
        CTS = 17,
        DT = 18, // 添加数据传输类型
        EndOfMsgAck = 19,
        BAM = 32,
        Abort = 255
    };

    // 终止原因
    enum class AbortReason : uint8_t
    {
        NO_RESOURCES = 1,
        TIMEOUT = 2,
        ALREADY_IN_PROCESS = 3,
        SYSTEM_ERROR = 4,
        BAD_SEQUENCE = 5
    };

} // namespace j1939sim

#endif // J1939_TYPES_HPP
