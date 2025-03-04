#ifndef J1939_SIMULATION_HPP
#define J1939_SIMULATION_HPP

#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include "j1939_types.hpp"
#include "j1939_sim.h"

namespace j1939sim
{

    // TP.CM PGN
    constexpr uint32_t PGN_TP_CM = 0x00EC00;
    // TP.DT PGN
    constexpr uint32_t PGN_TP_DT = 0x00EB00;

    struct ReceiveData
    {
        uint32_t id;
        std::vector<uint8_t> data;
    };

    struct NodeConfig
    {
        bool enable_tx{true};            // 是否开启发送
        bool enable_rx{true};            // 是否开启接收
        bool active{true};               // 节点是否激活
        uint32_t tp_packet_interval{50}; // 传输协议数据包间隔(毫秒)
        uint8_t max_cts_packets{8};      // 单次CTS请求的最大数据包数, 默认8
    };

    // 添加角色标识
    enum class SessionRole
    {
        SENDER,
        RECEIVER
    };

    // 会话标识结构
    struct SessionId
    {
        uint8_t src_addr;
        uint8_t dst_addr;
        SessionRole role;

        bool operator<(const SessionId &other) const
        {
            return std::tie(src_addr, dst_addr, role) <
                   std::tie(other.src_addr, other.dst_addr, other.role);
        }
    };

    struct TransportSession
    {
        // 会话标识信息
        uint8_t src_addr{0};
        uint8_t dst_addr{0};
        uint32_t pgn{0};
        uint8_t priority{7}; // 添加优先级字段，默认为7

        // 数据管理
        std::vector<uint8_t> data;
        size_t total_packets{0};
        uint8_t sequence_number{1};

        // 添加接收相关字段
        uint32_t total_size{0};      // 总数据大小
        size_t packets_received{0};  // 已接收的数据包数
        size_t packets_requested{0}; // 当前CTS请求的数据包数

        // 状态控制
        SessionState state{SessionState::WAIT_CTS};
        std::chrono::steady_clock::time_point next_action_time; // 保留这个字段
    };

    class J1939Simulation
    {
    public:
        J1939Simulation();
        ~J1939Simulation();

        bool init(Transmitter transmitter, void *context);

        bool transmit(uint32_t id, uint8_t *data, size_t length);

        bool onReceive(const std::vector<ReceiveData> &data);

        // Configuration
        void setNodeParams(uint8_t addr, const NodeConfig &config);

    private:
        Transmitter transmitter = nullptr;
        void *context = nullptr;

        // 获取节点配置
        NodeConfig getNodeConfig(uint8_t address);

        // Transport protocol handling
        bool handleTPDataTransfer(uint32_t id, const uint8_t *data, size_t length);
        bool handleTPConnectMangement(uint32_t id, const uint8_t *data, size_t length);
        bool sendRTS(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, uint32_t size, uint8_t total_packets, uint32_t pgn);
        bool sendCTS(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, uint8_t num_packets, uint8_t next_packet, uint32_t pgn);
        bool sendEndOfMsgAck(uint8_t src_addr, const TransportSession &session);
        bool sendBAM(uint8_t priority, uint8_t src_addr, uint32_t size, uint8_t total_packets, uint32_t pgn);
        bool sendDataPacket(const TransportSession &session, size_t packet_number);
        bool sendAbort(uint8_t dst_addr, uint8_t src_addr, uint32_t pgn, AbortReason reason);

        void processReceiveQueue();
        bool processReceiveMessage(const ReceiveData &msg);

        void processSessions();
        bool handleSession(std::shared_ptr<TransportSession> session, const NodeConfig &config);

    private:
        // 节点
        std::mutex node_mutex_;
        NodeConfig nodes_[255];

        // 接收消息队列
        std::queue<ReceiveData> receive_queue_;
        std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::atomic<bool> running_{true};
        std::thread receive_thread_;

        // 会话
        std::thread session_thread_; // Add this line
        std::map<SessionId, std::shared_ptr<TransportSession>> sessions_;
        std::mutex session_mutex_;
        std::condition_variable session_cv_;
        std::chrono::steady_clock::time_point next_session_check_;
        bool has_pending_sessions_{false};
    };

} // namespace j1939sim

#endif // J1939_SIMULATION_HPP
