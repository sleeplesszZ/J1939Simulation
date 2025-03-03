#include "j1939sim/j1939_simulation.hpp"
#include "j1939sim/j1939_types.hpp"
#include <thread>
#include <algorithm>

namespace j1939sim
{

    J1939Simulation::J1939Simulation()
    {
        next_session_check_ = std::chrono::steady_clock::now();
        // 启动会话处理线程
        session_thread_ = std::thread([this]()
                                      { processSessions(); });
        // 启动接收消息处理线程
        receive_thread_ = std::thread([this]()
                                      { processReceiveQueue(); });
    }

    J1939Simulation::~J1939Simulation()
    {
        running_ = false;
        queue_cv_.notify_one();
        session_cv_.notify_one();

        if (session_thread_.joinable())
        {
            session_thread_.join();
        }
        if (receive_thread_.joinable())
        {
            receive_thread_.join();
        }
    }

    bool J1939Simulation::init(Transmitter transmitter, void *context)
    {
        this->transmitter = transmitter;
        this->context = context;
        return true;
    }

    bool J1939Simulation::transmit(uint32_t id, uint8_t *data, size_t length)
    {
        uint8_t src_addr = id & 0xFF; // Fix: get source address from LSB
        uint8_t priority = (id >> 26) & 0x7;

        // 检查节点是否可以发送
        if (!node_manager_.canTransmit(src_addr))
        {
            return false;
        }

        // 获取节点配置
        NodeConfig *config = node_manager_.getNodeConfig(src_addr);
        if (!config)
        {
            return false;
        }

        if (length <= 8)
        {
            return transmitter(id, data, length, context);
        }

        // 从 CAN ID 中提取各个字段
        uint8_t edp = (id >> 24) & 0x1; // Extended Data Page
        uint8_t dp = (id >> 25) & 0x1;  // Data Page
        uint8_t pf = (id >> 16) & 0xFF; // PDU Format
        uint8_t ps = (id >> 8) & 0xFF;  // PDU Specific

        uint32_t pgn;
        uint8_t dst_addr;

        if (pf < 240) // PDU1 format
        {
            // PDU1: PGN = (EDP)(DP)(PF), PS field contains destination address
            pgn = (edp << 17) | (dp << 16) | (pf << 8);
            dst_addr = ps;
        }
        else // PDU2 format
        {
            // PDU2: PGN = (EDP)(DP)(PF)(PS)
            pgn = (edp << 17) | (dp << 16) | (pf << 8) | ps;
            dst_addr = 0xFF; // PDU2 messages are always destination specific
        }

        // Check BAM restrictions before creating session
        if (dst_addr == 0xFF && session_manager_.hasActiveBamSession(src_addr))
        {
            // Only one BAM session allowed per source
            return false;
        }

        // 使用 std::move 构造数据向量
        std::vector<uint8_t> data_vec(data, data + length);

        // 创建新的发送会话，使用 std::move 转移数据所有权
        auto session = session_manager_.createSession(src_addr, dst_addr, pgn,
                                                      priority, SessionRole::SENDER,
                                                      std::move(data_vec));
        if (!session)
        {
            return false;
        }

        bool result;
        if (session->is_bam)
        {
            result = sendBAM(*session);
            if (result)
            {
                session->state = SessionState::SENDING;
                session->current_timeout = J1939Timeouts::T3;
                // BAM延时设置为最小值50ms
                session->next_action_time = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(50);
            }
        }
        else
        {
            result = sendRTS(*session);
            if (result)
            {
                session->state = SessionState::WAIT_CTS;
                session->current_timeout = J1939Timeouts::T3;
                session->next_action_time = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(J1939Timeouts::T3);
            }
        }

        if (result)
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            has_pending_sessions_ = true;
            session_cv_.notify_one();
        }

        return result;
    }

    void J1939Simulation::processSessions()
    {
        while (running_)
        {
            std::unique_lock<std::mutex> lock(session_mutex_);

            // 等待直到下一个检查时间点或有新的会话需要处理
            auto now = std::chrono::steady_clock::now();
            session_cv_.wait_until(lock, next_session_check_, [this, now]()
                                   { return !running_ || has_pending_sessions_ || now >= next_session_check_; });

            if (!running_)
            {
                break;
            }

            checkAndScheduleSessions();
        }
    }

    void J1939Simulation::checkAndScheduleSessions()
    {
        auto ready_sessions = session_manager_.getReadySessions();
        std::chrono::steady_clock::time_point next_check =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1000); // 默认1秒后检查

        for (const auto &session_id : ready_sessions)
        {
            auto session = session_manager_.getSession(session_id.addr1,
                                                       session_id.addr2,
                                                       session_id.pgn,
                                                       session_id.role);

            if (session && node_manager_.canTransmit(session->src_addr))
            {
                if (!handleSession(session))
                {
                    session_manager_.removeSession(session_id.addr1,
                                                   session_id.addr2,
                                                   session_id.pgn,
                                                   session_id.role);
                }
                else
                {
                    // 更新下一次检查时间
                    next_check = std::min(next_check, session->next_action_time);
                }
            }
        }

        has_pending_sessions_ = false;
        next_session_check_ = next_check;
    }

    bool J1939Simulation::handleSession(std::shared_ptr<TransportSession> session)
    {
        auto config = node_manager_.getNodeConfig(session->src_addr);
        if (!config)
            return false;

        switch (session->state)
        {
        case SessionState::WAIT_CTS:
            // 检查超时
            if (std::chrono::steady_clock::now() - session->last_time >
                std::chrono::milliseconds(session->current_timeout))
            {
                sendAbort(session->dst_addr, session->src_addr, session->pgn, AbortReason::TIMEOUT); // 超时
                return false;
            }
            return true;

        case SessionState::SENDING:
            if (session->is_bam)
            {
                // BAM发送使用T3超时
                session->current_timeout = J1939Timeouts::T3;
                if (session->sequence_number <= session->total_packets)
                {
                    if (!sendDataPacket(*session, session->sequence_number++))
                    {
                        return false;
                    }
                    session->next_action_time = std::chrono::steady_clock::now() +
                                                std::chrono::milliseconds(50);
                    return true;
                }
                session->state = SessionState::COMPLETE; // 直接进入完成状态
                return false;                            // BAM完成
            }
            // 对于非BAM消息，等待CTS
            session->current_timeout = J1939Timeouts::T1;
            session->state = SessionState::WAIT_CTS;
            session->next_action_time = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(10);
            return true;

        case SessionState::RECEIVING:
        case SessionState::COMPLETE:
            return false;

        default:
            return false;
        }
    }

    bool J1939Simulation::onReceive(const std::vector<ReceiveData> &data_list)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (const auto &data : data_list)
            {
                receive_queue_.push(data);
            }
        }
        queue_cv_.notify_one();
        return true;
    }

    void J1939Simulation::processReceiveQueue()
    {
        while (running_)
        {
            std::vector<ReceiveData> messages;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]()
                               { return !receive_queue_.empty() || !running_; });

                if (!running_)
                {
                    break;
                }

                // 一次性获取队列中的所有消息
                while (!receive_queue_.empty())
                {
                    messages.push_back(std::move(receive_queue_.front()));
                    receive_queue_.pop();
                }
            }

            // 批量处理所有消息
            for (const auto &msg : messages)
            {
                processReceiveMessage(msg);
            }
        }
    }

    bool J1939Simulation::processReceiveMessage(const ReceiveData &msg)
    {
        uint8_t pf = (msg.id >> 16) & 0xFF;
        uint8_t ps = (msg.id >> 8) & 0xFF;

        // Determine destination address based on PDU format
        if (((pf < 240) && ps == 0xFF) || pf >= 240) // PDU1 format
        {
            return false;
        }

        uint8_t dst_addr = ps;
        // 检查节点是否可以接收
        if (!node_manager_.canReceive(dst_addr))
        {
            return false;
        }

        if (pf == (PGN_TP_CM >> 16))
        {
            return handleTPConnectMangement(msg.id, msg.data.data(), msg.data.size());
        }
        else if (pf == (PGN_TP_DT >> 16))
        {
            return handleTPDataTransfer(msg.id, msg.data.data(), msg.data.size());
        }

        return true;
    }

    bool J1939Simulation::handleTPDataTransfer(uint32_t id, const uint8_t *data, size_t length)
    {
        uint8_t src_addr = id & 0xFF;
        uint8_t dst_addr = (id >> 8) & 0xFF;

        std::shared_ptr<TransportSession> session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);

            if (dst_addr == 0xFF)
            {
                // For BAM, find the session by source address
                session = session_manager_.findActiveReceiveSession(src_addr, dst_addr, 0);
            }
            else
            {
                // For destination specific messages, allow multiple active sessions
                auto sessions = session_manager_.findActiveReceiveSessions(src_addr, dst_addr);
                if (sessions.size() > 1)
                {
                    // Multiple non-BAM sessions exist - abort them all
                    for (const auto &s : sessions)
                    {
                        sendAbort(s->dst_addr, s->src_addr, s->pgn, AbortReason::RESOURCES_BUSY); // 资源被占用
                    }
                    return false;
                }
                session = sessions.empty() ? nullptr : sessions[0];
            }

            if (!session || session->state != SessionState::RECEIVING)
            {
                return false;
            }

            uint8_t seq = data[0];
            if (seq != session->sequence_number)
            {
                sendAbort(session->src_addr, session->dst_addr, session->pgn, AbortReason::BAD_SEQUENCE); // 序列号错误
                return false;
            }

            // 保存数据
            size_t offset = (seq - 1) * 7;
            size_t remaining = session->total_size - offset;
            size_t data_length = std::min(remaining, size_t(7));

            // 确保数据缓冲区大小足够
            if (session->data.size() < offset + data_length)
            {
                session->data.resize(offset + data_length);
            }
            std::copy_n(data + 1, data_length, session->data.begin() + offset);

            session->sequence_number++;
            session->packets_received++;

            // 检查是否需要发送下一个CTS
            if (session->packets_received == session->packets_requested)
            {
                if (session->sequence_number <= session->total_packets)
                {
                    // 还有更多包需要接收
                    auto config = node_manager_.getNodeConfig(session->dst_addr);
                    if (!config)
                        return false;

                    uint8_t packets_to_request = std::min(
                        static_cast<uint8_t>(session->total_packets - session->sequence_number + 1),
                        config->max_cts_packets);

                    session->packets_requested = packets_to_request;
                    session->packets_received = 0;
                    return sendCTS(session, packets_to_request);
                }
                else
                {
                    // 所有包都已接收完成
                    session->state = SessionState::COMPLETE;
                    bool ack_result = sendEndOfMsgAck(session->src_addr, *session); // 更新函数调用

                    // 发送完EndOfMsgAck后清理会话
                    session_manager_.removeSession(session->src_addr,
                                                   session->dst_addr,
                                                   session->pgn,
                                                   SessionRole::RECEIVER);

                    return ack_result;
                }
            }
            return true;
        }
    }

    bool J1939Simulation::handleTPConnectMangement(uint32_t id, const uint8_t *data, size_t length)
    {
        uint8_t src_addr = id & 0xFF;
        uint8_t dst_addr = (id >> 8) & 0xFF;
        uint8_t priority = (id >> 26) & 0x7;
        TpCmType cmd = static_cast<TpCmType>(data[0]);
        uint32_t pgn = (data[6] << 16) | (data[5] << 8) | (data[4]);
        bool is_broadcast = (dst_addr == 0xFF); // 添加变量定义

        std::lock_guard<std::mutex> lock(session_mutex_);
        switch (cmd)
        {
        case TpCmType::RTS:
        {
            // Check if destination already has an active session
            if (session_manager_.hasActiveSession(dst_addr))
            {
                sendAbort(dst_addr, src_addr, pgn, AbortReason::RESOURCES_BUSY); // 资源被占用
                return false;
            }

            // Check if there's already a destination-specific session from this source
            if (!is_broadcast && session_manager_.hasActiveDestinationSpecificSession(src_addr, dst_addr))
            {
                sendAbort(dst_addr, src_addr, pgn, AbortReason::RESOURCES_BUSY); // 资源被占用
                return false;
            }

            // Check if node can handle another session
            auto config = node_manager_.getNodeConfig(dst_addr);
            if (!config || !node_manager_.canReceive(dst_addr))
            {
                sendAbort(dst_addr, src_addr, pgn, AbortReason::NO_RESOURCES); // 无可用资源
                return false;
            }

            uint32_t msg_size = data[1];
            uint8_t total_packets = data[2];

            auto session = session_manager_.createSession(src_addr, dst_addr, pgn, priority, SessionRole::RECEIVER);
            if (!session)
            {
                sendAbort(dst_addr, src_addr, pgn, AbortReason::NO_RESOURCES); // 无可用资源
                return false;
            }

            session->total_packets = total_packets;
            session->total_size = msg_size;
            session->state = SessionState::RECEIVING;
            session->last_time = std::chrono::steady_clock::now();
            session->sequence_number = 1;

            // 删除重复的config声明，使用上面已声明的config
            uint8_t packets_to_request = std::min(
                static_cast<uint8_t>(total_packets),
                config->max_cts_packets);
            session->packets_requested = packets_to_request;

            return sendCTS(session, packets_to_request);
        }
        case TpCmType::CTS:
        {
            auto session = session_manager_.getSession(src_addr, dst_addr, pgn, SessionRole::SENDER);
            if (session->state != SessionState::WAIT_CTS)
            {
                return false;
            }

            auto config = node_manager_.getNodeConfig(session->src_addr);
            if (!config)
                return false;

            uint8_t num_packets = data[1];
            uint8_t next_packet = data[2];

            session->packets_requested = num_packets;
            session->sequence_number = next_packet;
            session->state = SessionState::SENDING;
            session->current_timeout = J1939Timeouts::T3;
            session->last_time = std::chrono::steady_clock::now();

            // 使用节点配置的数据包间隔
            session->next_action_time = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(config->tp_packet_interval);
            return true;
        }
        case TpCmType::EndOfMsgAck:
        {
            auto session = session_manager_.getSession(src_addr, dst_addr, pgn, SessionRole::SENDER);
            session_manager_.removeSession(session->src_addr,
                                           session->dst_addr,
                                           session->pgn,
                                           SessionRole::SENDER);
            return true;
        }
        case TpCmType::Abort:
        {
            auto session = session_manager_.getSession(src_addr, dst_addr, pgn, SessionRole::SENDER);
            session_manager_.removeSession(session->src_addr,
                                           session->dst_addr,
                                           session->pgn,
                                           session->state == SessionState::WAIT_CTS ? SessionRole::SENDER : SessionRole::RECEIVER);
            return true;
        }
        case TpCmType::BAM:
        {
            if (!is_broadcast)
            {
                // BAM must be broadcast
                return false;
            }

            // For BAM, check if source already has an active BAM session
            if (session_manager_.hasActiveBamSession(src_addr))
            {
                // Silently ignore BAM if source already has active BAM
                return false;
            }

            uint32_t msg_size = data[1];
            uint8_t total_packets = data[2];

            auto session = session_manager_.createSession(src_addr, dst_addr, pgn,
                                                          priority, SessionRole::RECEIVER);
            if (!session)
            {
                return false;
            }

            session->total_packets = total_packets;
            session->total_size = msg_size;
            session->state = SessionState::RECEIVING;
            session->last_time = std::chrono::steady_clock::now();
            session->sequence_number = 1;
            session->is_bam = true;

            return true;
        }
        default:
            return false;
        }
    }

    bool J1939Simulation::sendRTS(const TransportSession &session)
    {
        // 按照SAE J1939协议规范构造RTS消息
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::RTS),             // Control byte = 16 (RTS)
            static_cast<uint8_t>(session.data.size()),       // Total message size (LSB)
            static_cast<uint8_t>(session.data.size() >> 8),  // Total message size (MSB)
            static_cast<uint8_t>(session.total_packets),     // Total number of packets
            0xFF,                                            // Maximum number of packets that can be sent (no limit)
            static_cast<uint8_t>(session.pgn & 0xFF),        // PGN byte 1 (LSB)
            static_cast<uint8_t>((session.pgn >> 8) & 0xFF), // PGN byte 2
            static_cast<uint8_t>((session.pgn >> 16) & 0xFF) // PGN byte 3 (MSB)
        };

        uint32_t id = (session.priority << 26) | (PGN_TP_CM << 8) | session.dst_addr;
        return transmitter(id, data, 8, context);
    }

    bool J1939Simulation::sendBAM(const TransportSession &session)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::BAM),
            static_cast<uint8_t>(session.data.size()),
            static_cast<uint8_t>(session.total_packets),
            0xFF,
            static_cast<uint8_t>(session.pgn & 0xFF),
            static_cast<uint8_t>((session.pgn >> 8) & 0xFF),
            static_cast<uint8_t>((session.pgn >> 16) & 0xFF),
            0xFF};

        uint32_t id = (session.priority << 26) | (PGN_TP_CM << 8) | 0xFF;
        return transmitter(id, data, 8, context);
    }

    bool J1939Simulation::sendDataPacket(const TransportSession &session, size_t packet_number)
    {
        uint8_t data[8] = {static_cast<uint8_t>(packet_number)};
        size_t offset = (packet_number - 1) * 7;
        size_t remaining = session.data.size() - offset;
        size_t length = std::min(remaining, size_t(7));

        std::copy_n(session.data.begin() + offset, length, data + 1);

        uint32_t id = (session.priority << 26) | (PGN_TP_DT << 8) |
                      (session.is_bam ? 0xFF : session.dst_addr);
        return transmitter(id, data, 8, context);
    }

    // 添加CTS发送函数
    bool J1939Simulation::sendCTS(const std::shared_ptr<TransportSession> &session, uint8_t num_packets)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::CTS),               // Control byte = CTS command
            num_packets,                                       // Number of packets that can be sent
            session->sequence_number,                          // Next packet number
            0xFF,                                              // Reserved
            static_cast<uint8_t>(session->pgn & 0xFF),         // PGN byte 1 (LSB)
            static_cast<uint8_t>((session->pgn >> 8) & 0xFF),  // PGN byte 2
            static_cast<uint8_t>((session->pgn >> 16) & 0xFF), // PGN byte 3 (MSB)
            0xFF                                               // Reserved
        };

        // CTS消息中，本地地址(session->dst_addr)应该在ID的低字节
        // 目标地址(session->src_addr)应该在PS字段
        uint32_t id = (session->priority << 26) | (PGN_TP_CM << 8) |
                      (session->src_addr << 8) | session->dst_addr;
        return transmitter(id, data, 8, context);
    }

    // 添加sendEndOfMsgAck函数
    bool J1939Simulation::sendEndOfMsgAck(uint8_t src_addr, const TransportSession &session)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::EndOfMsgAck),     // Control byte = 19
            static_cast<uint8_t>(session.total_size & 0xFF), // Total message size LSB
            static_cast<uint8_t>(session.total_size >> 8),   // Total message size MSB
            static_cast<uint8_t>(session.total_packets),     // Total number of packets
            0xFF,                                            // Reserved, must be 0xFF
            static_cast<uint8_t>(session.pgn & 0xFF),        // PGN byte 1 (LSB)
            static_cast<uint8_t>((session.pgn >> 8) & 0xFF), // PGN byte 2
            static_cast<uint8_t>((session.pgn >> 16) & 0xFF) // PGN byte 3 (MSB)
        };

        uint32_t id = (7 << 26) | (PGN_TP_CM << 8) | src_addr; // 使用默认优先级7
        return transmitter(id, data, 8, context);
    }

    // Add new overload of sendAbort that includes reason code
    bool J1939Simulation::sendAbort(uint8_t dst_addr, uint8_t src_addr, uint32_t pgn, AbortReason reason)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::Abort),
            static_cast<uint8_t>(reason), // Include abort reason
            0xFF,
            0xFF,
            static_cast<uint8_t>(pgn & 0xFF),
            static_cast<uint8_t>((pgn >> 8) & 0xFF),
            static_cast<uint8_t>((pgn >> 16) & 0xFF),
            0xFF};

        uint32_t id = (7 << 26) | (PGN_TP_CM << 8) | dst_addr;
        return transmitter(id, data, 8, context);
    }

} // namespace j1939sim
