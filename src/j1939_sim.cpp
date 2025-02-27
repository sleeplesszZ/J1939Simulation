#include "j1939sim/j1939_sim.h"
#include "j1939sim/j1939_simulation.hpp"

using namespace j1939sim;

extern "C"
{

    j1939_handle_t j1939_sim_create(void)
    {
        return new J1939Simulation();
    }

    void j1939_sim_destroy(j1939_handle_t handle)
    {
        delete static_cast<J1939Simulation *>(handle);
    }

    bool j1939_init(j1939_handle_t handle, Transmitter transmitter, void *context)
    {
        return static_cast<J1939Simulation *>(handle)->init(transmitter, context);
    }

    bool j1939_transmit(j1939_handle_t handle, uint32_t id, uint8_t *data, size_t length)
    {
        return static_cast<J1939Simulation *>(handle)->transmit(id, data, length);
    }

    bool j1939_on_receive(j1939_handle_t handle, uint32_t id, uint8_t *data, size_t length)
    {
        return static_cast<J1939Simulation *>(handle)->onReceive(id, data, length);
    }
}
