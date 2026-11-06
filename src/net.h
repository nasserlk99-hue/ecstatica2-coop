#ifndef NET_H
#define NET_H

#include "types.h"

/* Online co-op transport: host-authoritative UDP, client mirrors world state.
 *
 * Host runs the simulation for both players (local P1 + remote P2) and
 * broadcasts the world state every frame. Client runs locally, simulates
 * its own player, and interpolates the host's puppet. Host decides story
 * advance; client is read-only for scripts.
 */

#define NET_MAX_PACKET 4096
#define NET_PORT_DEFAULT 27015
#define NET_TIMEOUT_TICKS 600  /* 10s at 60Hz */

typedef enum {
    NET_OFFLINE = 0,
    NET_HOST = 1,
    NET_CLIENT = 2,
} net_mode_t;

typedef struct net_packet_s {
    char type; /* 'T' state, 'H' hello, 'A' ack, 'D' done */
    int seq;
    int tick;
    /* Actor wire state */
    int16_t thing;
    int16_t x, y, z;
    int16_t rx, ry, rz;
    int16_t hp;
    int16_t behavior;
    /* Input bits (host side: P1) */
    unsigned input_bits;
    /* Handshake */
    char name[16];
    int scene;
} net_packet_t;

typedef struct net_state_s {
    net_mode_t mode;
    bool active;
    bool connected;
    bool host_requested;   /* --host was passed */
    int port;
    char host_ip[16];
    int host_port;
    /* Client side: last host state + interpolation */
    int16_t p1_x, p1_y, p1_z, p1_rx, p1_ry, p1_rz;
    int16_t puppet_x, puppet_y, puppet_z;
    int16_t puppet_rx, puppet_ry, puppet_rz;
    int16_t puppet_prev_x, puppet_prev_z;
    int puppet_t;
    int last_recv_tick;
    int seq;
    unsigned last_input_bits;
} net_state_t;

extern net_state_t net_state;

void net_init(void);
void net_shutdown(void);
bool net_is_active(void);
net_mode_t net_mode(void);

/* called once per frame from coop_frame_update */
void net_frame_update(void);

/* Called by client to drain queued host packets into puppet state */
void net_apply_remote_state(void);

/* Send local input bits to host (client side) or broadcast state (host side) */
void net_send_input(unsigned bits);
void net_broadcast_state(int tick);

/* Host/join helpers */
bool net_start_host(int port);
bool net_join(const char *ip, int port);

/* Parse command-line args */
void net_parse_args(int argc, char *argv[]);

#endif /* NET_H */
