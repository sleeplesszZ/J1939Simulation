#include <j1939sim/j1939_sim.h>
#include <stdio.h>

int main()
{
    j1939_handle_t sim = j1939_sim_create();
    j1939_sim_set_source_address(sim, 0x80);

    // Send message
    uint8_t send_data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    j1939_sim_send_message(sim, 0x1000, 3, send_data, sizeof(send_data));

    // Receive message
    uint8_t recv_data[8];
    size_t length;
    if (j1939_sim_receive_message(sim, 0x1000, recv_data, &length) == 0)
    {
        printf("Received message with length: %zu\n", length);
        for (size_t i = 0; i < length; i++)
        {
            printf("Data[%zu]: %d\n", i, recv_data[i]);
        }
    }

    j1939_sim_destroy(sim);
    return 0;
}
