#ifndef J1939_SIM_H
#define J1939_SIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void *j1939_handle_t;

    // Create and destroy J1939 simulation instance
    j1939_handle_t j1939_sim_create(void);
    void j1939_sim_destroy(j1939_handle_t handle);

    typedef bool (*Transmitter)(uint32_t id, const uint8_t *data, size_t length, void *context);
    bool j1939_init(j1939_handle_t handle, Transmitter transmitter, void *context);

    bool j1939_transmit(j1939_handle_t handle, uint32_t id, uint8_t *data, size_t length);
    bool j1939_on_receive(j1939_handle_t handle, uint32_t id, uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // J1939_SIM_H
