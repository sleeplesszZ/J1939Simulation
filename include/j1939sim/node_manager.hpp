#ifndef NODE_MANAGER_HPP
#define NODE_MANAGER_HPP

#include "node_config.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <chrono>

namespace j1939sim
{

    class NodeManager
    {
    public:
        // 添加或更新节点配置
        void setNodeConfig(uint8_t address, const NodeConfig &config)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            nodes_[address] = config;
        }

        // 获取节点配置
        NodeConfig *getNodeConfig(uint8_t address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodes_.find(address);
            return it != nodes_.end() ? &it->second : nullptr;
        }

        // 检查节点是否可以发送
        bool canTransmit(uint8_t address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodes_.find(address);
            return (it != nodes_.end() && it->second.active && it->second.enable_tx);
        }

        // 检查节点是否可以接收
        bool canReceive(uint8_t address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodes_.find(address);
            return (it != nodes_.end() && it->second.active && it->second.enable_rx);
        }

        // 获取所有活跃节点
        std::vector<uint8_t> getActiveNodes()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<uint8_t> active_nodes;
            for (const auto &[addr, config] : nodes_)
            {
                if (config.active)
                {
                    active_nodes.push_back(addr);
                }
            }
            return active_nodes;
        }

    private:
        std::map<uint8_t, NodeConfig> nodes_;
        std::mutex mutex_;
    };

} // namespace j1939sim

#endif // NODE_MANAGER_HPP
