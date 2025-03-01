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
        COMPLETE
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

    // 删除 AbortReason 枚举类型

} // namespace j1939sim

#endif // J1939_TYPES_HPP
