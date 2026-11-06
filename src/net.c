/**
 * net.c
 *
 * Online co-op transport: host-authoritative UDP, client mirrors world state.
 */

#include "net.h"
#include "player.h"
#include "game.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

net_state_t net_state;

static SOCKET sock = 0;
static struct sockaddr_in host_addr;
static bool initialized;
static int last_state_tick;

void net_init(void) {
    memset(&net_state, 0, sizeof(net_state));
    net_state.mode = NET_OFFLINE;
    net_state.active = false;
    net_state.connected = false;
    net_state.port = NET_PORT_DEFAULT;
    initialized = true;
    sock = (SOCKET)-1;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

void net_shutdown(void) {
    if (sock != (SOCKET)-1) {
        closesocket(sock);
        sock = (SOCKET)-1;
    }
    net_state.active = false;
    net_state.connected = false;
    net_state.mode = NET_OFFLINE;
#ifdef _WIN32
    WSACleanup();
#endif
}

bool net_is_active(void) { return net_state.active; }
net_mode_t net_mode(void) { return net_state.mode; }

void net_parse_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            net_state.port = atoi(argv[i+1]);
            net_state.host_requested = true;
        } else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
            const char *arg = argv[i+1];
            /* Format: ip[:port] */
            const char *colon = strchr(arg, ':');
            if (colon) {
                int len = (int)(colon - arg);
                if (len >= 15) len = 15;
                strncpy(net_state.host_ip, arg, len);
                net_state.host_ip[len] = 0;
                net_state.host_port = atoi(colon + 1);
            } else {
                strncpy(net_state.host_ip, arg, 15);
                net_state.host_ip[15] = 0;
                net_state.host_port = NET_PORT_DEFAULT;
            }
        }
    }
}

static bool net_open_socket(int port) {
    if (sock != (SOCKET)-1) {
        closesocket(sock);
        sock = (SOCKET)-1;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == (SOCKET)-1) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        sock = (SOCKET)-1;
        return false;
    }
    /* Non-blocking */
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    if (net_state.mode == NET_HOST) {
        /* Host listens on port, no specific peer yet */
        host_addr = addr;
    } else if (net_state.mode == NET_CLIENT) {
        /* Client sends to host; addr is destination */
        memset(&host_addr, 0, sizeof(host_addr));
        host_addr.sin_family = AF_INET;
        host_addr.sin_port = htons((uint16_t)net_state.host_port);
        if (inet_pton(AF_INET, net_state.host_ip, &host_addr.sin_addr) <= 0) {
            /* Try inet_addr fallback */
            host_addr.sin_addr.s_addr = inet_addr(net_state.host_ip);
            if (host_addr.sin_addr.s_addr == INADDR_NONE) {
                closesocket(sock);
                sock = (SOCKET)-1;
                return false;
            }
        }
        if (connect(sock, (struct sockaddr *)&host_addr, sizeof(host_addr)) == SOCKET_ERROR) {
            closesocket(sock);
            sock = (SOCKET)-1;
            return false;
        }
    }
    return true;
}

bool net_start_host(int port) {
    net_state.mode = NET_HOST;
    net_state.port = port;
    if (!net_open_socket(port)) return false;
    net_state.active = true;
    net_state.connected = true;
    DBG_LOG(1, "[NET] hosting on port %d\n", port);
    return true;
}

bool net_join(const char *ip, int port) {
    strncpy(net_state.host_ip, ip, 15);
    net_state.host_ip[15] = 0;
    net_state.host_port = port;
    net_state.mode = NET_CLIENT;
    if (!net_open_socket(0)) return false;  /* ephemeral local port */
    net_state.active = true;
    net_state.connected = false;
    DBG_LOG(1, "[NET] joining %s:%d\n", ip, port);
    return true;
}

void net_send_input(unsigned bits) {
    if (!net_state.active || sock == (SOCKET)-1) return;
    if (net_state.mode != NET_CLIENT) return;

    char buf[128];
    /* Client input packet: type I + bits + tick + seq */
    int len = snprintf(buf, sizeof(buf), "I,%d,%d,%u", net_state.seq++, last_state_tick, bits);
    if (len > 0)
        send(sock, buf, len, 0);
}

void net_broadcast_state(int tick) {
    if (!net_state.active || sock == (SOCKET)-1) return;
    if (net_state.mode != NET_HOST) return;

    actor_t *p1 = coop_players[0].actor;
    actor_t *p2 = coop_players[1].actor;
    if (!p1 || !p2) return;
    /* No client address known yet (still INADDR_ANY): wait, don't spam fails. */
    if (host_addr.sin_addr.s_addr == 0) return;

    /* Host broadcasts both players' state so client can mirror */
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "S,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
        net_state.seq++, tick,
        (int)p1->name_index, (int)p1->position_vector.X, (int)p1->position_vector.Y, (int)p1->position_vector.Z,
        (int)p1->rotate_vector.X, (int)p1->rotate_vector.Y, (int)p1->rotate_vector.Z,
        (int)p1->actor_hitpoints, (int)p1->actor_behavior,
        (int)p2->name_index, (int)p2->position_vector.X, (int)p2->position_vector.Y, (int)p2->position_vector.Z,
        (int)p2->rotate_vector.X, (int)p2->rotate_vector.Y, (int)p2->rotate_vector.Z,
        (int)p2->actor_hitpoints, (int)p2->actor_behavior, (int)game_timer);
    if (len > 0) {
        int sent = sendto(sock, buf, len, 0, (struct sockaddr *)&host_addr, sizeof(host_addr));
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            DBG_LOG(1, "[NET] sendto failed: %d (host_addr=%s:%d)\n", err, inet_ntoa(host_addr.sin_addr), ntohs(host_addr.sin_port));
        }
    }
}

void net_apply_remote_state(void) {
    if (!net_state.active || sock == (SOCKET)-1) return;

    char buf[4096];
    struct sockaddr_in src;
    int srclen = sizeof(src);
    int n;

    /* Drain all pending datagrams */
    while ((n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &srclen)) > 0) {
        buf[n] = 0;
        if (buf[0] == 'H') {
            /* Hello / reconnect */
            if (net_state.mode == NET_HOST) {
                /* Host acknowledges client reconnection */
                net_state.connected = true;
                DBG_LOG(1, "[NET] client connected from %s\n", inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf)));
                /* Record client address for state broadcasts */
                host_addr.sin_family = AF_INET;
                host_addr.sin_port = src.sin_port;
                host_addr.sin_addr = src.sin_addr;
            } else if (net_state.mode == NET_CLIENT) {
                /* Client acknowledges host is back */
                net_state.connected = true;
                DBG_LOG(1, "[NET] host reconnected\n");
            }
        } else if (buf[0] == 'S' && net_state.mode == NET_CLIENT) {
            /* Host state packet */
            int seq, tick;
            int t1, x1, y1, z1, rx1, ry1, rz1, hp1, bh1;
            int t2, x2, y2, z2, rx2, ry2, rz2, hp2, bh2, gt;
            if (sscanf(buf, "S,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                &seq, &tick, &t1, &x1, &y1, &z1, &rx1, &ry1, &rz1, &hp1, &bh1,
                &t2, &x2, &y2, &z2, &rx2, &ry2, &rz2, &hp2, &bh2, &gt) == 21) {
                /* Client mirrors host's P1 (host player) and P2 (own char) */
                net_state.p1_x = (int16_t)x1;
                net_state.p1_y = (int16_t)y1;
                net_state.p1_z = (int16_t)z1;
                net_state.p1_rx = (int16_t)rx1;
                net_state.p1_ry = (int16_t)ry1;
                net_state.p1_rz = (int16_t)rz1;
                net_state.puppet_prev_x = net_state.puppet_x;
                net_state.puppet_prev_z = net_state.puppet_z;
                net_state.puppet_x = (int16_t)x2;
                net_state.puppet_y = (int16_t)y2;
                net_state.puppet_z = (int16_t)z2;
                net_state.puppet_rx = (int16_t)rx2;
                net_state.puppet_ry = (int16_t)ry2;
                net_state.puppet_rz = (int16_t)rz2;
                net_state.puppet_t = 0;
                net_state.connected = true;
                net_state.last_recv_tick = tick;
                last_state_tick = tick;
                DBG_LOG(1, "[NET] client recv state p1=(%d,%d) p2=(%d,%d)\n", x1, z1, x2, z2);
            }
        } else if (buf[0] == 'I' && net_state.mode == NET_HOST) {
            /* Client input packet - forward to P2 */
            int seq, tick;
            unsigned bits;
            if (sscanf(buf, "I,%d,%d,%u", &seq, &tick, &bits) == 3) {
                net_state.last_input_bits = bits;
                net_state.connected = true;
                DBG_LOG(1, "[NET] host recv input bits=%u peer=%s:%d\n", bits,
                        inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf)), ntohs(src.sin_port));
                /* Record client address for state broadcasts */
                host_addr.sin_family = AF_INET;
                host_addr.sin_port = src.sin_port;
                host_addr.sin_addr = src.sin_addr;
            }
        }
    }
}

void net_frame_update(void) {
    if (!net_state.active) return;
    net_apply_remote_state();

    if (net_state.mode == NET_CLIENT) {
        /* Handle disconnect/reconnect */
        if (net_state.connected && game_time - net_state.last_recv_tick > 180) {
            net_state.connected = false;
            DBG_LOG(1, "[NET] client timed out - waiting for reconnection\n");
        }
        if (!net_state.connected) {
            /* Try to reconnect with hello */
            if (sock != (SOCKET)-1) {
                char hello[64];
                int len = snprintf(hello, sizeof(hello), "H,%d", net_state.seq++);
                if (len > 0) send(sock, hello, len, 0);
            }
        }
    }

    if (net_state.mode == NET_HOST) {
        /* Host broadcasts state every frame */
        net_broadcast_state(last_state_tick + 1);
    } else if (net_state.mode == NET_CLIENT) {
        /* Client stays silent until input changes */
        net_send_input(net_state.last_input_bits);
    }
}
