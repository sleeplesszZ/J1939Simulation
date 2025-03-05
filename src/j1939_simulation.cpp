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

    void J1939Simulation::setNodeParams(uint8_t addr, const NodeConfig &config)
    {
        std::lock_guard<std::mutex> lock(node_mutex_);
        nodes_[addr] = config;
    }

    NodeConfig J1939Simulation::getNodeConfig(uint8_t address)
    {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return nodes_[address];
    }

    bool J1939Simulation::transmit(uint32_t id, uint8_t *data, size_t length)
    {
        uint8_t src_addr = id & 0xFF;
        // 检查节点是否可以发送
        auto config = getNodeConfig(src_addr);
        if (!config.active || !config.enable_tx)
        {
            return false;
        }

        if (length <= 8)
        {
            return transmitter(id, data, length, context);
        }

        // 从 CAN ID 中提取各个字段
        uint8_t priority = (id >> 26) & 0x7; // 优先级
        uint8_t edp = (id >> 24) & 0x1;      // Extended Data Page
        uint8_t dp = (id >> 25) & 0x1;       // Data Page
        uint8_t pf = (id >> 16) & 0xFF;      // PDU Format
        uint8_t ps = (id >> 8) & 0xFF;       // PDU Specific

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

        bool result = false;
        SessionId sid{src_addr, dst_addr, SessionRole::SENDER};
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto it = sessions_.find(sid);
            if (it != sessions_.end())
            {
                return false;
            }
            // 创建新的会话
            auto session = std::make_shared<TransportSession>();
            session->src_addr = src_addr;
            session->dst_addr = dst_addr;
            session->pgn = pgn;
            session->priority = priority;
            session->data.assign(data, data + length);
            session->total_packets = (length + 6) / 7;
            if (dst_addr == 0xFF)
            {
                session->state = SessionState::SENDING;
                session->next_action_time = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(config.tp_packet_interval);
                result = sendBAM(priority, src_addr, session->data.size(), session->total_packets, pgn);
            }
            else
            {
                session->state = SessionState::WAIT_CTS;
                session->next_action_time = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(J1939Timeouts::T1);
                result = sendRTS(priority, src_addr, dst_addr, session->data.size(), session->total_packets, pgn);
            }
            has_pending_sessions_ = true;
        }

        if (result)
        {
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

            now = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point next_check =
                now + std::chrono::milliseconds(1000); // 默认1秒后检查
            auto it = sessions_.begin();
            while (it != sessions_.end())
            {
                auto session = it->second;
                if (!session)
                {
                    it = sessions_.erase(it);
                    continue;
                }

                if (now < session->next_action_time)
                {
                    it++;
                    continue;
                }

                NodeConfig config;
                if (it->first.role == SessionRole::RECEIVER)
                {
                    config = getNodeConfig(session->dst_addr);
                    if (config.active || !config.enable_rx)
                    {
                        it++;
                        continue;
                    }
                }
                else
                {
                    config = getNodeConfig(session->src_addr);
                    if (!config.active || !config.enable_tx)
                    {
                        it++;
                        continue;
                    }
                }
                if (!handleSession(session, config))
                {
                    it = sessions_.erase(it);
                    continue;
                }
                // 更新下一次检查时间
                next_check = std::min(next_check, session->next_action_time);
                it++;
            }

            has_pending_sessions_ = false;
            next_session_check_ = next_check;
        }
    }

    bool J1939Simulation::handleSession(std::shared_ptr<TransportSession> session, const NodeConfig &config)
    {
        switch (session->state)
        {
        case SessionState::WAIT_CTS:
            sendAbort(session->priority, session->dst_addr, session->src_addr, session->pgn, AbortReason::TIMEOUT); // 超时
            return false;
        case SessionState::SENDING:
        {
            // 广播消息
            if (session->dst_addr == 0xFF)
            {
                if (session->sequence_number <= session->total_packets)
                {
                    // 修改这里：使用新的sendDataPacket函数签名
                    if (!sendDataPacket(session->priority, session->src_addr,
                                        session->dst_addr, session->sequence_number++,
                                        session->data))
                    {
                        return false;
                    }
                    // 设置下一个数据包的发送时间
                    session->next_action_time = std::chrono::steady_clock::now() +
                                                std::chrono::milliseconds(config.tp_packet_interval);
                    return true;
                }
                session->state = SessionState::COMPLETE; // 直接进入完成状态
                return false;                            // BAM完成
            }
            // 对于非BAM消息，等待CTS
            session->state = SessionState::WAIT_CTS;
            session->next_action_time = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(10);
            return true;
        }
        case SessionState::RECEIVING:
        case SessionState::COMPLETE:
            return false;

        default:
            return false;
        }
    }

    bool J1939Simulation::onReceive(uint32_t id, uint8_t *data, size_t length)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            ReceiveData rd;
            rd.id = id;
            rd.data.assign(data, data + length);
            receive_queue_.push(std::move(rd));
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
        NodeConfig config = getNodeConfig(dst_addr);
        if (!config.active || !config.enable_rx)
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
        if (length != 8)
            return false;

        // 从CAN ID中提取地址信息
        uint8_t src_addr = id & 0xFF;
        uint8_t dst_addr = (id >> 8) & 0xFF;
        uint8_t sequence = data[0];

        // 创建会话标识
        SessionId sid{src_addr, dst_addr, SessionRole::RECEIVER};

        std::lock_guard<std::mutex> lock(session_mutex_);

        // 查找对应的会话
        auto it = sessions_.find(sid);
        if (it == sessions_.end())
            return false;

        auto session = it->second;
        if (!session || session->state != SessionState::RECEIVING)
            return false;

        // 验证序列号是否符合预期
        if (sequence != session->sequence_number)
        {
            // 序列号错误，中止传输
            sendAbort(session->priority, dst_addr, src_addr, session->pgn, AbortReason::BAD_SEQUENCE);
            sessions_.erase(it);
            return false;
        }

        // 计算数据偏移量和本次数据长度
        size_t offset = (sequence - 1) * 7;
        size_t data_length = std::min(size_t(7), session->total_size - offset);

        // 确保数据缓冲区足够大
        if (session->data.size() < offset + data_length)
            session->data.resize(session->total_size);

        // 复制数据
        std::copy_n(data + 1, data_length, session->data.begin() + offset);

        session->packets_received++;
        session->sequence_number++;

        // 检查是否需要发送新的CTS
        if (session->packets_received == session->packets_requested &&
            session->packets_received < session->total_packets)
        {
            // 计算剩余需要接收的数据包数量
            uint8_t remaining_packets = session->total_packets - session->packets_received;
            auto config = getNodeConfig(dst_addr);
            uint8_t packets_to_request = std::min({
                remaining_packets,       // 不超过剩余包数
                config.max_cts_packets,  // 不超过本地CTS限制
                session->rts_max_packets // 不超过发送方指定的限制
            });

            // 发送CTS请求下一组数据包
            session->packets_requested += packets_to_request;
            session->next_action_time = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(J1939Timeouts::T2);
            return sendCTS(session->priority, dst_addr, src_addr,
                           packets_to_request, session->sequence_number, session->pgn);
        }

        // 检查是否接收完成
        if (session->packets_received == session->total_packets)
        {
            // 发送结束确认
            bool result = sendEndOfMsgAck(session->priority, dst_addr, src_addr,
                                          session->total_size, session->total_packets,
                                          session->pgn);
            sessions_.erase(it);
            return result;
        }

        // 更新下一次超时检查时间
        session->next_action_time = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(J1939Timeouts::T1);
        return true;
    }

    bool J1939Simulation::handleTPConnectMangement(uint32_t id, const uint8_t *data, size_t length)
    {
        auto pf = (id >> 16) & 0xFF;
        uint8_t dst_addr = (id >> 8) & 0xFF;
        if ((pf > 240) || (pf < 240 && dst_addr != 0xFF))
        {
            // 广播消息无需处理
            return false;
        }
        uint8_t src_addr = id & 0xFF;
        uint8_t priority = (id >> 26) & 0x7;
        TpCmType cmd = static_cast<TpCmType>(data[0]);
        uint32_t pgn = (data[6] << 16) | (data[5] << 8) | (data[4]);
        bool is_broadcast = (dst_addr == 0xFF); // 添加变量定义

        auto config = getNodeConfig(dst_addr);
        if (!config.active || !config.enable_rx)
        {
            return false;
        }

        switch (cmd)
        {
        case TpCmType::RTS:
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            uint32_t msg_size = data[1];
            uint8_t total_packets = data[2];
            uint8_t rts_max_packets = data[4]; // 获取RTS中的最大包数限制
            auto sid = SessionId{src_addr, dst_addr, SessionRole::RECEIVER};

            auto it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                // 创建新会话
                auto session = std::make_shared<TransportSession>();
                session->priority = priority;
                session->pgn = pgn;
                session->src_addr = src_addr;
                session->dst_addr = dst_addr;
                session->total_packets = total_packets;
                session->sequence_number = 1;
                session->total_size = msg_size;
                session->packets_received = 0;
                session->rts_max_packets = rts_max_packets; // 保存RTS中的最大包数限制
                uint8_t packets_to_request = std::min({
                    static_cast<uint8_t>(total_packets), // 不超过总包数
                    config.max_cts_packets,              // 不超过本地CTS限制
                    rts_max_packets                      // 不超过发送方指定的限制
                });
                session->packets_requested = packets_to_request;
                session->state = SessionState::RECEIVING;
                session->next_action_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(J1939Timeouts::T2);
                sessions_[sid] = session;
                return sendCTS(priority, dst_addr, src_addr, packets_to_request, 1, pgn);
            }

            auto session = sessions_[sid];
            if (!session)
            {
                sessions_.erase(it);
                return false;
            }
            // 已存在相同的会话，但PGN不同
            if (pgn != session->pgn)
            {
                sendAbort(priority, dst_addr, src_addr, pgn, AbortReason::INCOMPLETE_TRANSFER); // 未完成的传输
                return false;
            }

            // 已存在相同的会话，PGN相同，使用最新RTS，无需发送abort
            session->priority = priority;
            session->pgn = pgn;
            session->src_addr = src_addr;
            session->dst_addr = dst_addr;
            session->total_packets = total_packets;
            session->sequence_number = 1;
            session->total_size = msg_size;
            session->packets_received = 0;
            session->rts_max_packets = rts_max_packets; // 更新RTS中的最大包数限制
            uint8_t packets_to_request = std::min({
                static_cast<uint8_t>(total_packets), // 不超过总包数
                config.max_cts_packets,              // 不超过本地CTS限制
                rts_max_packets                      // 不超过发送方指定的限制
            });
            session->packets_requested = packets_to_request;
            session->state = SessionState::RECEIVING;
            session->next_action_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(J1939Timeouts::T2);
            return sendCTS(priority, dst_addr, src_addr, packets_to_request, 1, pgn);
        }
        case TpCmType::CTS:
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto sid = SessionId{dst_addr, src_addr, SessionRole::SENDER};
            auto it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            auto session = sessions_[sid];
            if (!session)
            {
                sessions_.erase(it);
                return false;
            }
            if (session->state != SessionState::WAIT_CTS)
            {
                return false;
            }

            uint8_t num_packets = data[1];
            uint8_t next_packet = data[2];

            session->packets_requested = num_packets;
            session->sequence_number = next_packet;
            session->state = SessionState::SENDING;
            session->next_action_time = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(config.tp_packet_interval);
            return true;
        }
        case TpCmType::EndOfMsgAck:
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto sid = SessionId{dst_addr, src_addr, SessionRole::SENDER};
            auto it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            return true;
        }
        case TpCmType::Abort:
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto sid = SessionId{dst_addr, src_addr, SessionRole::SENDER};
            auto it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            sid = SessionId{dst_addr, src_addr, SessionRole::RECEIVER};
            it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            sid = SessionId{src_addr, dst_addr, SessionRole::SENDER};
            it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            sid = SessionId{src_addr, dst_addr, SessionRole::RECEIVER};
            it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                return false;
            }
            return true;
        }
        case TpCmType::BAM:
        {
            return true;
        }
        default:
            return false;
        }
    }

    bool J1939Simulation::sendRTS(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, size_t total_size, uint8_t total_packets, uint32_t pgn)
    {
        // 获取源地址节点的配置以使用其max_rts_packets值
        auto config = getNodeConfig(src_addr);

        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::RTS),            // Control byte = 16 (RTS)
            static_cast<uint8_t>(total_size & 0xFF),        // Total message size LSB
            static_cast<uint8_t>((total_size >> 8) & 0xFF), // Total message size MSB
            total_packets,                                  // Total number of packets
            config.max_rts_packets,                         // Maximum number of packets that can be sent
            static_cast<uint8_t>(pgn & 0xFF),               // PGN byte 1 (LSB)
            static_cast<uint8_t>((pgn >> 8) & 0xFF),        // PGN byte 2
            static_cast<uint8_t>((pgn >> 16) & 0xFF)        // PGN byte 3 (MSB)
        };

        uint32_t id = (priority << 26) | (PGN_TP_CM << 8) | dst_addr;
        return transmitter(id, data, 8, context);
    }

    bool J1939Simulation::sendBAM(uint8_t priority, uint8_t src_addr, size_t total_size, uint8_t total_packets, uint32_t pgn)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::BAM),            // Control byte = 32 (BAM)
            static_cast<uint8_t>(total_size & 0xFF),        // Total message size LSB
            static_cast<uint8_t>((total_size >> 8) & 0xFF), // Total message size MSB
            total_packets,                                  // Total number of packets
            static_cast<uint8_t>(pgn & 0xFF),               // PGN byte 1 (LSB)
            static_cast<uint8_t>((pgn >> 8) & 0xFF),        // PGN byte 2
            static_cast<uint8_t>((pgn >> 16) & 0xFF),       // PGN byte 3 (MSB)
            0xFF                                            // Reserved
        };

        uint32_t id = (priority << 26) | (PGN_TP_CM << 8) | 0xFF; // BAM always broadcasts (0xFF)
        return transmitter(id, data, 8, context);
    }

    bool J1939Simulation::sendDataPacket(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, size_t packet_number, const std::vector<uint8_t> &data)
    {
        uint8_t packet[8] = {static_cast<uint8_t>(packet_number)};
        size_t offset = (packet_number - 1) * 7;
        size_t remaining = data.size() - offset;
        size_t length = std::min(remaining, size_t(7));

        std::copy_n(data.begin() + offset, length, packet + 1);

        uint32_t id = (priority << 26) | (PGN_TP_DT << 8) |
                      (src_addr << 8) | dst_addr;
        return transmitter(id, packet, 8, context);
    }

    // 添加sendEndOfMsgAck函数
    bool J1939Simulation::sendEndOfMsgAck(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, size_t total_size, uint8_t total_packets, uint32_t pgn)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::EndOfMsgAck), // Control byte = 19
            static_cast<uint8_t>(total_size & 0xFF),     // Total message size LSB
            static_cast<uint8_t>(total_size >> 8),       // Total message size MSB
            static_cast<uint8_t>(total_packets),         // Total number of packets
            0xFF,                                        // Reserved, must be 0xFF
            static_cast<uint8_t>(pgn & 0xFF),            // PGN byte 1 (LSB)
            static_cast<uint8_t>((pgn >> 8) & 0xFF),     // PGN byte 2
            static_cast<uint8_t>((pgn >> 16) & 0xFF)     // PGN byte 3 (MSB)
        };

        uint32_t id = (priority << 26) | (PGN_TP_CM << 8) | (src_addr << 8) | dst_addr; // 使用默认优先级7
        return transmitter(id, data, 8, context);
    }

    // Add new overload of sendAbort that includes reason code
    bool J1939Simulation::sendAbort(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, uint32_t pgn, AbortReason reason)
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

        uint32_t id = (priority << 26) | (PGN_TP_CM << 8) | (src_addr << 8) | dst_addr;
        return transmitter(id, data, 8, context);
    }

    bool J1939Simulation::sendCTS(uint8_t priority, uint8_t src_addr, uint8_t dst_addr, uint8_t num_packets, uint8_t next_packet, uint32_t pgn)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::CTS),      // Control byte = 17 (CTS)
            num_packets,                              // Number of packets that can be sent
            next_packet,                              // Next packet number to be sent
            0xFF,                                     // Reserved
            static_cast<uint8_t>(pgn & 0xFF),         // PGN byte 1 (LSB)
            static_cast<uint8_t>((pgn >> 8) & 0xFF),  // PGN byte 2
            static_cast<uint8_t>((pgn >> 16) & 0xFF), // PGN byte 3 (MSB)
            0xFF                                      // Reserved
        };

        // CTS消息中，本地地址(dst_addr)应该在ID的低字节
        // 目标地址(src_addr)应该在PS字段
        uint32_t id = (priority << 26) | (PGN_TP_CM << 8) | (src_addr << 8) | dst_addr;
        return transmitter(id, data, 8, context);
    }

} // namespace j1939sim
