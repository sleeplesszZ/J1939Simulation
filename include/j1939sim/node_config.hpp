#ifndef NODE_CONFIG_HPP
#define NODE_CONFIG_HPP

namespace j1939sim
{

    struct NodeConfig
    {
        bool enable_tx{true};            // 是否开启发送
        bool enable_rx{true};            // 是否开启接收
        bool active{true};               // 节点是否激活
        uint32_t tp_packet_interval{50}; // 传输协议数据包间隔(毫秒)
        uint8_t max_cts_packets{8};      // 单次CTS请求的最大数据包数, 默认8
    };

} // namespace j1939sim

#endif // NODE_CONFIG_HPP
