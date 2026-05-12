#ifndef MSG_H
#define MSG_H

#include <ctime>
#include <cstdint>

#define MAX_NAME_LEN 32
#define MAX_DATA_LEN 1024
#define MAX_TIME_STR_LEN 64

enum MsgType {
    T_HELLO        = 1,
    T_WELCOME      = 2,
    T_TEXT         = 3,
    T_PING         = 4,
    T_PONG         = 5,
    T_BYE          = 6,
    T_AUTH         = 7,
    T_PRIVATE      = 8,
    T_ERR          = 9,
    T_SRV_INFO     = 10,
    T_LIST         = 11,
    T_HIST         = 12,
    T_HIST_DATA    = 13,
    T_HELP         = 14,
    T_ACK          = 15
};

struct Msg {
    uint32_t len;
    uint8_t typ;
    uint32_t id;
    char from[MAX_NAME_LEN];
    char to[MAX_NAME_LEN];
    time_t ts;
    char data[MAX_DATA_LEN];
};

struct PendMsg {
    Msg m;
    time_t sent_at;
    int tries;
    float rtt_us;
};

struct OffMsg {
    char from[MAX_NAME_LEN];
    char to[MAX_NAME_LEN];
    char txt[MAX_DATA_LEN];
    uint32_t id;
    time_t ts;
};

struct PingRes {
    uint32_t id;
    long long rtt_ms;
    bool ok;
    long long jit_ms;
};

#endif
