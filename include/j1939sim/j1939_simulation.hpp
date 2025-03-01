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
#include "async_worker.hpp"
#include "transport_session_manager.hpp"
#include "node_manager.hpp"

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

    class J1939Simulation
    {
    public:
        J1939Simulation();
        ~J1939Simulation();

        bool init(Transmitter transmitter, void *context);

        bool transmit(uint32_t id, uint8_t *data, size_t length);

        bool onReceive(const std::vector<ReceiveData> &data);

        // Configuration
        void setNodeParams(uint8_t address, uint32_t timeout_ms = 1000, uint32_t packet_delay_ms = 10);

        // 节点配置接口
        void setNodeConfig(uint8_t address, const NodeConfig &config)
        {
            node_manager_.setNodeConfig(address, config);
        }

        NodeConfig *getNodeConfig(uint8_t address)
        {
            return node_manager_.getNodeConfig(address);
        }

        std::vector<uint8_t> getActiveNodes()
        {
            return node_manager_.getActiveNodes();
        }

    private:
        Transmitter transmitter = nullptr;
        void *context = nullptr;

        // Transport protocol handling
        bool handleTPDataTransfer(uint32_t id, const uint8_t *data, size_t length);
        bool handleTPConnectMangement(uint32_t id, const uint8_t *data, size_t length);
        bool sendRTS(const TransportSession &session);
        bool sendCTS(uint8_t src_addr, uint8_t num_packets, uint8_t next_packet);
        bool sendEndOfMsgAck(uint8_t src_addr);
        bool sendBAM(const TransportSession &session);
        bool sendDataPacket(const TransportSession &session, size_t packet_number);
        bool sendAbort(uint8_t dst_addr, uint8_t src_addr, uint32_t pgn);

        void processTransportSessions();
        bool processSession(std::shared_ptr<TransportSession> session);
        void scheduleNextCheck(std::shared_ptr<TransportSession> session,
                               std::chrono::milliseconds delay);

        std::unique_ptr<AsyncWorker> worker_;
        TransportSessionManager session_manager_;
        NodeManager node_manager_;

        // 添加接收消息队列相关
        std::queue<ReceiveData> receive_queue_;
        std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::atomic<bool> running_{true};
        std::thread receive_thread_;

        void startReceiveProcessor();
        void processReceiveQueue();
        bool processReceiveMessage(const ReceiveData &msg);

        // 添加用于会话处理的同步原语
        std::mutex session_mutex_;
        std::condition_variable session_cv_;
        std::chrono::steady_clock::time_point next_session_check_;
        bool has_pending_sessions_{false};

        void checkAndScheduleSessions();
        void wakeupSessionProcessor();

        bool handleCTS(std::shared_ptr<TransportSession> session, const uint8_t *data);
    };

} // namespace j1939sim

#endif // J1939_SIMULATION_HPP
