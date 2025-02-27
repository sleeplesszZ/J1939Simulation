#include <j1939sim/j1939_message.hpp>
#include <iostream>

int main()
{
    j1939sim::J1939Message msg(0x1000, 3, 0x80);

    uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    msg.setData(data, sizeof(data));

    std::cout << "PGN: 0x" << std::hex << msg.getPGN() << std::endl;
    std::cout << "Priority: " << std::dec << (int)msg.getPriority() << std::endl;
    std::cout << "Source: 0x" << std::hex << (int)msg.getSourceAddress() << std::endl;

    return 0;
}
