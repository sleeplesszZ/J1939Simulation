#ifndef TRANSPORT_SESSION_MANAGER_HPP
#define TRANSPORT_SESSION_MANAGER_HPP

#include <map>
#include <queue>
#include <memory>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include "j1939_simulation.hpp"
#include "j1939_types.hpp"

namespace j1939sim
{

    // Remove SessionState enum as it's now in j1939_types.hpp

    namespace J1939Timeouts
    {
        constexpr uint32_t T1 = 750;  // Time between CTS messages received from the destination (ms)
        constexpr uint32_t T2 = 1250; // Time between RTS and first CTS received (ms)
        constexpr uint32_t T3 = 1250; // Time limit for receiver to send next CTS (ms)
        constexpr uint32_t T4 = 1050; // Time between last data packet and EndOfMsgAck (ms)
        constexpr uint32_t Tr = 200;  // Time between data packets received (ms)
        constexpr uint32_t Th = 500;  // Time to hold unused connection resources (ms)
    }

    // 添加角色标识
    enum class SessionRole
    {
        SENDER,
        RECEIVER
    };

    // 会话标识结构
    struct SessionId
    {
        uint8_t addr1;    // 第一个节点地址
        uint8_t addr2;    // 第二个节点地址
        SessionRole role; // 本节点的角色
        uint32_t pgn;     // 消息PGN

        bool operator<(const SessionId &other) const
        {
            return std::tie(addr1, addr2, role, pgn) <
                   std::tie(other.addr1, other.addr2, other.role, other.pgn);
        }
    };

    struct TransportSession
    {
        // 会话标识信息
        uint8_t src_addr{0};
        uint8_t dst_addr{0};
        uint32_t pgn{0};
        uint8_t priority{7}; // 添加优先级字段，默认为7
        bool is_bam{false};

        // 数据管理
        std::vector<uint8_t> data;
        size_t total_packets{0};
        uint8_t sequence_number{1};

        // 添加接收相关字段
        uint32_t total_size{0};      // 总数据大小
        size_t packets_received{0};  // 已接收的数据包数
        size_t packets_requested{0}; // 当前CTS请求的数据包数

        // 状态控制
        SessionState state{SessionState::WAIT_CTS}; // 修改默认状态
        std::chrono::steady_clock::time_point last_time;
        std::chrono::steady_clock::time_point next_action_time;
        uint32_t current_timeout{J1939Timeouts::T2};
    };

    class TransportSessionManager
    {
    public:
        using Clock = std::chrono::steady_clock;

        // 修改会话创建函数签名，使用右值引用
        std::shared_ptr<TransportSession> createSession(uint8_t src_addr,
                                                        uint8_t dst_addr,
                                                        uint32_t pgn,
                                                        uint8_t priority,
                                                        SessionRole role,
                                                        std::vector<uint8_t> &&data = std::vector<uint8_t>())
        {
            // 删除锁,由外层控制并发访问
            SessionId id{src_addr, dst_addr, role, pgn};

            auto it = sessions_.find(id);
            if (it != sessions_.end())
            {
                sessions_.erase(it);
            }

            auto session = std::make_shared<TransportSession>();
            session->src_addr = src_addr;
            session->dst_addr = dst_addr;
            session->pgn = pgn;
            session->priority = priority;
            session->last_time = Clock::now();
            session->next_action_time = session->last_time;

            // 发送方特有的初始化，使用 std::move 转移数据所有权
            if (role == SessionRole::SENDER)
            {
                session->data = std::move(data);
                session->total_packets = (session->data.size() + 6) / 7;
                session->is_bam = (dst_addr == 0xFF);
            }

            sessions_[id] = session;
            return session;
        }

        std::shared_ptr<TransportSession> getSession(uint8_t addr1,
                                                     uint8_t addr2,
                                                     uint32_t pgn,
                                                     SessionRole role)
        {
            // 删除shared_lock
            auto it = sessions_.find(SessionId{addr1, addr2, role, pgn});
            return (it != sessions_.end()) ? it->second : nullptr;
        }

        void removeSession(uint8_t addr1, uint8_t addr2, uint32_t pgn, SessionRole role)
        {
            sessions_.erase(SessionId{addr1, addr2, role, pgn});
        }

        std::vector<SessionId> getReadySessions()
        {
            std::vector<SessionId> ready_sessions;
            auto now = Clock::now();
            for (const auto &[id, session] : sessions_)
            {
                if (now >= session->next_action_time)
                {
                    ready_sessions.push_back(id);
                }
            }
            return ready_sessions;
        }

        std::shared_ptr<TransportSession> findActiveReceiveSession(uint8_t src_addr, uint8_t dst_addr, uint32_t pgn)
        {
            for (const auto &[id, session] : sessions_)
            {
                if (id.role == SessionRole::RECEIVER &&
                    session->src_addr == src_addr &&
                    session->dst_addr == dst_addr &&
                    session->pgn == pgn && // Also match PGN
                    session->state == SessionState::RECEIVING)
                {
                    return session;
                }
            }
            return nullptr;
        }

        bool hasActiveSession(uint8_t dst_addr) const
        {
            // Check if destination address already has any active sessions
            for (const auto &[id, session] : sessions_)
            {
                if (session->dst_addr == dst_addr &&
                    session->state != SessionState::COMPLETE)
                {
                    return true;
                }
            }
            return false;
        }

        bool hasActiveSessionFromSource(uint8_t src_addr, uint8_t dst_addr) const
        {
            // Check if there's already an active session between these addresses
            for (const auto &[id, session] : sessions_)
            {
                if (session->src_addr == src_addr &&
                    session->dst_addr == dst_addr &&
                    session->state != SessionState::COMPLETE)
                {
                    return true;
                }
            }
            return false;
        }

        bool hasActiveDestinationSpecificSession(uint8_t src_addr, uint8_t dst_addr) const
        {
            // Check if there's already a destination-specific session between these addresses
            for (const auto &[id, session] : sessions_)
            {
                if (session->src_addr == src_addr &&
                    session->dst_addr == dst_addr &&
                    !session->is_bam && // Only check non-BAM sessions
                    session->state != SessionState::COMPLETE)
                {
                    return true;
                }
            }
            return false;
        }

        bool hasActiveBamSession(uint8_t src_addr) const
        {
            // Check if source address already has any active BAM sessions
            for (const auto &[id, session] : sessions_)
            {
                if (session->src_addr == src_addr &&
                    session->is_bam &&
                    session->state != SessionState::COMPLETE)
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::shared_ptr<TransportSession>> findActiveReceiveSessions(uint8_t src_addr, uint8_t dst_addr)
        {
            std::vector<std::shared_ptr<TransportSession>> active_sessions;
            for (const auto &[id, session] : sessions_)
            {
                if (id.role == SessionRole::RECEIVER &&
                    session->src_addr == src_addr &&
                    session->dst_addr == dst_addr &&
                    session->state == SessionState::RECEIVING)
                {
                    active_sessions.push_back(session);
                }
            }
            return active_sessions;
        }

    private:
        std::map<SessionId, std::shared_ptr<TransportSession>> sessions_;
        // 删除mutex_和shared_mutex_
    };

} // namespace j1939sim

#endif // TRANSPORT_SESSION_MANAGER_HPP
