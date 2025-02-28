#include "j1939sim/j1939_simulation.hpp"
#include "j1939sim/j1939_types.hpp"
#include <thread>
#include <algorithm>

namespace j1939sim
{

    J1939Simulation::J1939Simulation() : worker_(std::make_unique<AsyncWorker>())
    {
        next_session_check_ = std::chrono::steady_clock::now();
        // 只提交一次任务，后续通过条件变量控制
        worker_->submit([this]()
                        { processTransportSessions(); }, std::chrono::milliseconds(0));

        // 启动接收消息处理线程
        startReceiveProcessor();
    }

    J1939Simulation::~J1939Simulation()
    {
        running_ = false;
        queue_cv_.notify_one();
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
        uint8_t src_addr = (id >> 8) & 0xFF;
        uint8_t priority = (id >> 26) & 0x7; // 从输入id中提取优先级

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

        uint8_t dst_addr = id & 0xFF;
        uint32_t pgn = id & 0x3FFFF00;

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

        session->state = SessionState::INIT;
        session->current_timeout = session->is_bam ? J1939Timeouts::T3 : J1939Timeouts::T2;

        // 唤醒会话处理器
        wakeupSessionProcessor();

        return session->is_bam ? sendBAM(*session) : sendRTS(*session);
    }

    void J1939Simulation::processTransportSessions()
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
        bool need_reschedule = false;
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
                if (!processSession(session))
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
                    need_reschedule = true;
                }
            }
        }

        has_pending_sessions_ = false;
        next_session_check_ = next_check;
    }

    void J1939Simulation::wakeupSessionProcessor()
    {
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            has_pending_sessions_ = true;
        }
        session_cv_.notify_one();
    }

    bool J1939Simulation::processSession(std::shared_ptr<TransportSession> session)
    {
        // 获取节点配置以获取发送间隔
        auto config = node_manager_.getNodeConfig(session->src_addr);
        if (!config)
            return false;

        switch (session->state)
        {
        case SessionState::INIT:
            if (session->is_bam)
            {
                for (size_t i = 0; i < std::min(size_t(8), session->total_packets); ++i)
                {
                    if (!sendDataPacket(*session, session->sequence_number++))
                    {
                        return false;
                    }
                    // 使用节点配置的数据包间隔
                    std::this_thread::sleep_for(std::chrono::milliseconds(config->tp_packet_interval));
                }
                scheduleNextCheck(session, std::chrono::milliseconds(config->tp_packet_interval));
                session->state = SessionState::SENDING;
            }
            return true;

        case SessionState::WAIT_CTS:
            // 检查超时
            if (std::chrono::steady_clock::now() - session->last_time >
                std::chrono::milliseconds(session->current_timeout))
            {
                return false;
            }
            scheduleNextCheck(session, std::chrono::milliseconds(10));
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
                    // 使用节点配置的数据包间隔
                    scheduleNextCheck(session, std::chrono::milliseconds(config->tp_packet_interval));
                    return true;
                }
                return false; // BAM完成
            }
            // 对于非BAM消息，等待CTS
            session->current_timeout = J1939Timeouts::T1;
            session->state = SessionState::WAIT_CTS;
            scheduleNextCheck(session, std::chrono::milliseconds(10));
            return true;

        case SessionState::WAIT_ACK:
            // 等待EndOfMsgAck使用T4超时
            session->current_timeout = J1939Timeouts::T4;
            if (std::chrono::steady_clock::now() - session->last_time >
                std::chrono::milliseconds(session->current_timeout))
            {
                return false;
            }
            return true;

        default:
            return false;
        }
    }

    void J1939Simulation::scheduleNextCheck(std::shared_ptr<TransportSession> session,
                                            std::chrono::milliseconds delay)
    {
        session->next_action_time = std::chrono::steady_clock::now() + delay;
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

    void J1939Simulation::startReceiveProcessor()
    {
        receive_thread_ = std::thread([this]()
                                      { processReceiveQueue(); });
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
        uint8_t dst_addr = msg.id & 0xFF;

        // 检查节点是否可以接收
        if (!node_manager_.canReceive(dst_addr))
        {
            return false;
        }

        if (((msg.id >> 16) & 0xFF) == (PGN_TP_CM >> 16))
        {
            return handleTransportProtocol(msg.id, msg.data.data(), msg.data.size());
        }

        return true;
    }

    bool J1939Simulation::handleTransportProtocol(uint32_t id, const uint8_t *data, size_t length)
    {
        uint8_t src_addr = id & 0xFF;
        uint8_t dst_addr = (id >> 8) & 0xFF;
        uint8_t priority = (id >> 26) & 0x7; // 提取优先级
        TpCmType cmd = static_cast<TpCmType>(data[0]);
        uint32_t pgn = (data[6] << 16) | (data[5] << 8) | (data[4]);

        auto session = session_manager_.findSessionByCanId(id, data);
        if (!session)
        {
            if (cmd == TpCmType::RTS)
            {
                uint32_t msg_size = data[1];
                uint8_t total_packets = data[2];

                // 创建接收会话
                session = session_manager_.createSession(src_addr, dst_addr, pgn,
                                                         priority, SessionRole::RECEIVER);
                if (!session)
                {
                    sendAbort(dst_addr, src_addr, pgn, AbortReason::NO_RESOURCES);
                    return false;
                }

                // 初始化接收会话
                session->total_packets = total_packets;
                session->total_size = msg_size;
                session->state = SessionState::RECEIVING; // 修改为正确的接收状态
                session->last_time = std::chrono::steady_clock::now();
                session->sequence_number = 1; // 从第一个包开始接收

                // 获取节点配置，决定一次请求多少包
                auto config = node_manager_.getNodeConfig(dst_addr);
                if (!config)
                    return false;

                uint8_t packets_to_request = std::min(static_cast<uint8_t>(total_packets - session->sequence_number + 1),
                                                      config->max_cts_packets);
                session->packets_requested = packets_to_request; // 保存请求的包数

                // 发送首个CTS
                return sendCTS(src_addr, packets_to_request, session->sequence_number);
            }
            return false;
        }

        switch (cmd)
        {
        case TpCmType::CTS:
        {
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

            // 使用节点配置的数据包间隔
            scheduleNextCheck(session, std::chrono::milliseconds(config->tp_packet_interval));
            return true;
        }
        case TpCmType::EndOfMsgAck:
        {
            // 移除会话时需要指定正确的角色
            session_manager_.removeSession(session->src_addr,
                                           session->dst_addr,
                                           session->pgn,
                                           SessionRole::SENDER);
            return true;
        }
        case TpCmType::Abort:
        {
            // 收到Abort时移除相应的会话
            session_manager_.removeSession(session->src_addr,
                                           session->dst_addr,
                                           session->pgn,
                                           session->state == SessionState::WAIT_CTS ? SessionRole::SENDER : SessionRole::RECEIVER);
            return true;
        }
        case TpCmType::DT:
        {
            if (session->state != SessionState::RECEIVING)
            {
                return false;
            }

            uint8_t seq = data[0];
            if (seq != session->sequence_number)
            {
                sendAbort(session->src_addr, session->dst_addr, session->pgn, AbortReason::BAD_SEQUENCE);
                return false;
            }

            // 保存数据
            size_t offset = (seq - 1) * 7;
            size_t remaining = session->total_size - offset;
            size_t length = std::min(remaining, size_t(7));

            // 确保数据缓冲区大小足够
            if (session->data.size() < offset + length)
            {
                session->data.resize(offset + length);
            }
            std::copy_n(data + 1, length, session->data.begin() + offset);

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

                    uint8_t packets_to_request = std::min(static_cast<uint8_t>(session->total_packets - session->sequence_number + 1),
                                                          config->max_cts_packets);

                    session->packets_requested = packets_to_request; // 更新请求的包数
                    session->packets_received = 0;                   // 重置接收计数
                    return sendCTS(session->src_addr, packets_to_request, session->sequence_number);
                }
                else
                {
                    // 所有包都已接收完成
                    session->state = SessionState::COMPLETE;
                    bool ack_result = sendEndOfMsgAck(session->src_addr);

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
        default:
            return false;
        }
    }

    bool J1939Simulation::sendRTS(const TransportSession &session)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::RTS),
            static_cast<uint8_t>(session.data.size()),
            static_cast<uint8_t>(session.total_packets),
            0xFF,
            static_cast<uint8_t>(session.pgn & 0xFF),
            static_cast<uint8_t>((session.pgn >> 8) & 0xFF),
            static_cast<uint8_t>((session.pgn >> 16) & 0xFF),
            0xFF};

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
    bool J1939Simulation::sendCTS(uint8_t src_addr, uint8_t num_packets, uint8_t next_packet)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::CTS),
            num_packets, // 请求的数据包数量
            next_packet, // 下一个期望的数据包序号
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF};

        uint32_t id = (7 << 26) | (PGN_TP_CM << 8) | src_addr; // 使用默认优先级7
        return transmitter(id, data, 8, context);
    }

    // 添加缺失的abort发送函数
    bool J1939Simulation::sendAbort(uint8_t dst_addr, uint8_t src_addr, uint32_t pgn, AbortReason reason)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::Abort),
            static_cast<uint8_t>(reason),
            0xFF,
            0xFF,
            static_cast<uint8_t>(pgn & 0xFF),
            static_cast<uint8_t>((pgn >> 8) & 0xFF),
            static_cast<uint8_t>((pgn >> 16) & 0xFF),
            0xFF};

        uint32_t id = (7 << 26) | (PGN_TP_CM << 8) | dst_addr; // 使用默认优先级7
        return transmitter(id, data, 8, context);
    }

    // 添加sendEndOfMsgAck函数
    bool J1939Simulation::sendEndOfMsgAck(uint8_t src_addr)
    {
        uint8_t data[8] = {
            static_cast<uint8_t>(TpCmType::EndOfMsgAck),
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF};

        uint32_t id = (7 << 26) | (PGN_TP_CM << 8) | src_addr; // 使用默认优先级7
        return transmitter(id, data, 8, context);
    }

} // namespace j1939sim
