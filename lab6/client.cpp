#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctime>
#include <iomanip>
#include <map>
#include <vector>
#include <fstream>
#include <chrono>
#include "messages.h"

static uint32_t g_msg_id = 1;
static pthread_mutex_t g_id_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pend_mtx = PTHREAD_MUTEX_INITIALIZER;

uint32_t gen_id() {
    pthread_mutex_lock(&g_id_mtx);
    uint32_t id = g_msg_id++;
    pthread_mutex_unlock(&g_id_mtx);
    return id;
}

#define PORT 8888
#define RECONN_DELAY 2
#define ACK_TO 2
#define MAX_RETRY 3

using namespace std;

int sock_fd = -1;
bool conn = false;
char nick[MAX_NAME_LEN];
pthread_mutex_t sock_mtx = PTHREAD_MUTEX_INITIALIZER;

map<uint32_t, PendMsg> pend_msgs;
pthread_t retrans_thr;
bool retrans_run = true;

map<uint32_t, chrono::steady_clock::time_point> ping_times;
map<uint32_t, PingRes> ping_res;
vector<long long> rtt_vals;
vector<long long> jitter_vals;
int ping_sent = 0;
int ping_recvd = 0;

string fmt_time(time_t t) {
    char buf[MAX_TIME_STR_LEN];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return string(buf);
}

int send_msg(int fd, const Msg& m) {
    uint32_t len_net = htonl(m.len);
    
    if (send(fd, &len_net, 4, 0) != 4) return -1;
    if (send(fd, &m.typ, 1, 0) != 1) return -1;
    if (send(fd, &m.id, 4, 0) != 4) return -1;
    if (send(fd, m.from, MAX_NAME_LEN, 0) != MAX_NAME_LEN) return -1;
    if (send(fd, m.to, MAX_NAME_LEN, 0) != MAX_NAME_LEN) return -1;
    if (send(fd, &m.ts, sizeof(time_t), 0) != (ssize_t)sizeof(time_t)) return -1;
    if (send(fd, m.data, m.len - 1, 0) != (ssize_t)(m.len - 1)) return -1;
    
    return 0;
}

int recv_msg(int fd, Msg& m) {
    uint32_t len_net;
    if (recv(fd, &len_net, 4, 0) != 4) return -1;
    m.len = ntohl(len_net);
    
    if (recv(fd, &m.typ, 1, 0) != 1) return -1;
    if (recv(fd, &m.id, 4, 0) != 4) return -1;
    if (recv(fd, m.from, MAX_NAME_LEN, 0) != MAX_NAME_LEN) return -1;
    if (recv(fd, m.to, MAX_NAME_LEN, 0) != MAX_NAME_LEN) return -1;
    if (recv(fd, &m.ts, sizeof(time_t), 0) != (ssize_t)sizeof(time_t)) return -1;
    if (recv(fd, m.data, m.len - 1, 0) != (ssize_t)(m.len - 1)) return -1;
    m.data[m.len - 1] = '\0';
    
    return 0;
}

void add_pend(const Msg& m) {
    pthread_mutex_lock(&g_pend_mtx);
    PendMsg p;
    p.m = m;
    p.sent_at = time(nullptr);
    p.tries = 0;
    p.rtt_us = 0;
    pend_msgs[m.id] = p;
    pthread_mutex_unlock(&g_pend_mtx);
}

void rem_pend(uint32_t id) {
    pthread_mutex_lock(&g_pend_mtx);
    pend_msgs.erase(id);
    pthread_mutex_unlock(&g_pend_mtx);
}

void resend_msg(uint32_t id) {
    pthread_mutex_lock(&g_pend_mtx);
    auto it = pend_msgs.find(id);
    if (it != pend_msgs.end()) {
        it->second.tries++;
        it->second.sent_at = time(nullptr);
        
        cout << "[Transport][RETRY] resend " << it->second.tries << "/" << MAX_RETRY 
             << " (id=" << id << ")" << endl;
        
        pthread_mutex_lock(&sock_mtx);
        if (conn && sock_fd != -1) {
            send_msg(sock_fd, it->second.m);
        }
        pthread_mutex_unlock(&sock_mtx);
    }
    pthread_mutex_unlock(&g_pend_mtx);
}

void* retransmit_proc(void* arg) {
    while (retrans_run) {
        sleep(1);
        
        time_t now = time(nullptr);
        pthread_mutex_lock(&g_pend_mtx);
        
        vector<uint32_t> to_resend, to_remove;
        
        for (auto& p : pend_msgs) {
            uint32_t id = p.first;
            PendMsg& pend = p.second;
            
            if (now - pend.sent_at >= ACK_TO) {
                if (pend.tries >= MAX_RETRY) {
                    cout << "[Transport][RETRY] lost (id=" << id 
                         << ") after " << MAX_RETRY << " attempts" << endl;
                    to_remove.push_back(id);
                } else {
                    to_resend.push_back(id);
                }
            }
        }
        
        for (uint32_t id : to_resend) resend_msg(id);
        for (uint32_t id : to_remove) pend_msgs.erase(id);
        
        pthread_mutex_unlock(&g_pend_mtx);
    }
    return nullptr;
}

bool connect_srv() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    
    Msg hello = {};
    hello.typ = T_HELLO;
    hello.id = 1;
    hello.ts = time(nullptr);
    strncpy(hello.from, "Client", MAX_NAME_LEN - 1);
    strncpy(hello.data, "Client", MAX_DATA_LEN - 1);
    hello.len = strlen(hello.data) + 1;
    
    if (send_msg(fd, hello) != 0) {
        close(fd);
        return false;
    }
    
    Msg welcome = {};
    if (recv_msg(fd, welcome) != 0 || welcome.typ != T_WELCOME) {
        close(fd);
        return false;
    }
    
    cout << welcome.data << endl;
    
    cout << "Enter nickname: ";
    cin.getline(nick, MAX_NAME_LEN);
    
    Msg auth = {};
    auth.typ = T_AUTH;
    auth.id = gen_id();
    auth.ts = time(nullptr);
    strncpy(auth.from, nick, MAX_NAME_LEN - 1);
    strncpy(auth.data, nick, MAX_DATA_LEN - 1);
    auth.len = strlen(auth.data) + 1;
    
    if (send_msg(fd, auth) != 0) {
        close(fd);
        return false;
    }
    
    Msg auth_resp = {};
    if (recv_msg(fd, auth_resp) != 0) {
        close(fd);
        return false;
    }
    
    if (auth_resp.typ == T_ERR) {
        cout << "Auth failed: " << auth_resp.data << endl;
        close(fd);
        return false;
    }
    
    pthread_mutex_lock(&sock_mtx);
    sock_fd = fd;
    conn = true;
    pthread_mutex_unlock(&sock_mtx);
    
    return true;
}

void save_diag(const char* name, double avg_rtt, double avg_jit, double loss) {
    char fname[256];
    snprintf(fname, sizeof(fname), "net_diag_%s.json", name);
    
    ofstream file(fname);
    if (file.is_open()) {
        file << "{" << endl;
        file << "  \"nick\": \"" << name << "\"," << endl;
        file << "  \"ts\": " << time(nullptr) << "," << endl;
        file << "  \"avg_rtt_ms\": " << avg_rtt << "," << endl;
        file << "  \"avg_jit_ms\": " << avg_jit << "," << endl;
        file << "  \"loss_pct\": " << loss << "," << endl;
        file << "  \"sent\": " << ping_sent << "," << endl;
        file << "  \"recvd\": " << ping_recvd << endl;
        file << "}" << endl;
        file.close();
        cout << "[Transport][DIAG] saved to " << fname << endl;
    }
}

void handle_pong(uint32_t id) {
    auto it = ping_times.find(id);
    if (it != ping_times.end()) {
        auto now = chrono::steady_clock::now();
        long long rtt = chrono::duration_cast<chrono::milliseconds>(now - it->second).count();
        
        PingRes res;
        res.id = id;
        res.rtt_ms = rtt;
        res.ok = true;
        
        static long long last_rtt = -1;
        if (last_rtt != -1 && !rtt_vals.empty()) {
            res.jit_ms = abs(rtt - last_rtt);
            jitter_vals.push_back(res.jit_ms);
            cout << "PING " << id << " → RTT=" << rtt << "ms | Jitter=" << res.jit_ms << "ms" << endl;
        } else {
            res.jit_ms = 0;
            cout << "PING " << id << " → RTT=" << rtt << "ms" << endl;
        }
        
        last_rtt = rtt;
        rtt_vals.push_back(rtt);
        ping_res[id] = res;
        ping_recvd++;
        ping_times.erase(it);
    }
}

void send_pings(int cnt) {
    ping_sent = 0;
    ping_recvd = 0;
    rtt_vals.clear();
    jitter_vals.clear();
    ping_times.clear();
    ping_res.clear();
    
    for (int i = 0; i < cnt; i++) {
        uint32_t pid = gen_id();
        
        Msg ping = {};
        ping.typ = T_PING;
        ping.id = pid;
        ping.ts = time(nullptr);
        strncpy(ping.from, nick, MAX_NAME_LEN - 1);
        strncpy(ping.data, "ping", MAX_DATA_LEN - 1);
        ping.len = strlen(ping.data) + 1;
        
        pthread_mutex_lock(&sock_mtx);
        if (conn && sock_fd != -1) {
            ping_times[pid] = chrono::steady_clock::now();
            send_msg(sock_fd, ping);
            ping_sent++;
        }
        pthread_mutex_unlock(&sock_mtx);
        
        usleep(100000);
    }
    
    sleep(2);
    
    for (auto& p : ping_times) {
        uint32_t id = p.first;
        cout << "PING " << id << " → timeout" << endl;
        
        PingRes res;
        res.id = id;
        res.rtt_ms = 0;
        res.ok = false;
        res.jit_ms = 0;
        ping_res[id] = res;
    }
}

void show_diag() {
    if (rtt_vals.empty()) {
        cout << "No ping data. Run /ping first." << endl;
        return;
    }
    
    double avg_rtt = 0;
    for (long long r : rtt_vals) avg_rtt += r;
    avg_rtt /= rtt_vals.size();
    
    double avg_jit = 0;
    if (!jitter_vals.empty()) {
        for (long long j : jitter_vals) avg_jit += j;
        avg_jit /= jitter_vals.size();
    }
    
    double loss = 100.0 - (100.0 * ping_recvd / ping_sent);
    
    cout << "\n========================================" << endl;
    cout << "Network Diagnostics:" << endl;
    cout << "========================================" << endl;
    cout << "RTT avg : " << fixed << setprecision(1) << avg_rtt << " ms" << endl;
    cout << "Jitter  : " << fixed << setprecision(1) << avg_jit << " ms" << endl;
    cout << "Loss    : " << fixed << setprecision(1) << loss << "%" << endl;
    cout << "========================================" << endl;
    
    save_diag(nick, avg_rtt, avg_jit, loss);
}

void* recv_proc(void* arg) {
    while (true) {
        pthread_mutex_lock(&sock_mtx);
        int fd = sock_fd;
        bool is_conn = conn;
        pthread_mutex_unlock(&sock_mtx);
        
        if (!is_conn || fd == -1) break;
        
        Msg m = {};
        int res = recv_msg(fd, m);
        
        if (res != 0) {
            pthread_mutex_lock(&sock_mtx);
            close(sock_fd);
            sock_fd = -1;
            conn = false;
            pthread_mutex_unlock(&sock_mtx);
            cout << "\n[CLIENT]: Lost connection. Reconnecting..." << endl;
            break;
        }
        
        if (m.typ == T_ACK) {
            uint32_t acked = atoi(m.data);
            cout << "[Transport][ACK] recvd (id=" << acked << ")" << endl;
            rem_pend(acked);
        }
        else if (m.typ == T_PONG) {
            cout << "[Transport][PONG] recvd (id=" << m.id << ")" << endl;
            handle_pong(m.id);
        }
        else if (m.typ == T_TEXT) {
            cout << "\n[" << fmt_time(m.ts) << "][id=" << m.id 
                 << "][" << m.from << "]: " << m.data << endl;
            cout << "> " << flush;
        }
        else if (m.typ == T_PRIVATE) {
            cout << "\n[" << fmt_time(m.ts) << "][id=" << m.id 
                 << "][PVT][" << m.from << " -> " << m.to << "]: " << m.data << endl;
            cout << "> " << flush;
        }
        else if (m.typ == T_SRV_INFO) {
            cout << "\n" << m.data << endl;
            cout << "> " << flush;
        }
        else if (m.typ == T_ERR) {
            cout << "\n[ERROR]: " << m.data << endl;
            cout << "> " << flush;
        }
        else if (m.typ == T_HIST_DATA) {
            cout << "\n[HISTORY]:\n" << m.data << "> " << flush;
        }
    }
    return nullptr;
}

int main() {
    char buf[512];
    pthread_t recv_thr;
    bool exit_flag = false;
    
    pthread_create(&retrans_thr, nullptr, retransmit_proc, nullptr);
    
    while (!exit_flag) {
        if (!conn) {
            if (connect_srv()) {
                pthread_create(&recv_thr, nullptr, recv_proc, nullptr);
                pthread_detach(recv_thr);
                cout << "\nCommands: /help, /list, /history, /history N, /quit, /ping, /ping N, /netdiag, /w <nick> <msg>" << endl;
                cout << "> " << flush;
            } else {
                cout << "Connection failed. Retry in " << RECONN_DELAY << "s..." << endl;
                sleep(RECONN_DELAY);
                continue;
            }
        }
        
        cin.getline(buf, sizeof(buf));
        
        pthread_mutex_lock(&sock_mtx);
        int fd = sock_fd;
        bool is_conn = conn;
        pthread_mutex_unlock(&sock_mtx);
        
        if (!is_conn || fd == -1) continue;
        
        if (strcmp(buf, "/quit") == 0) {
            Msg bye = {};
            bye.typ = T_BYE;
            bye.id = gen_id();
            bye.ts = time(nullptr);
            strncpy(bye.from, nick, MAX_NAME_LEN - 1);
            strncpy(bye.data, "bye", MAX_DATA_LEN - 1);
            bye.len = strlen(bye.data) + 1;
            send_msg(fd, bye);
            
            pthread_mutex_lock(&sock_mtx);
            close(sock_fd);
            sock_fd = -1;
            conn = false;
            pthread_mutex_unlock(&sock_mtx);
            exit_flag = true;
            cout << "Disconnected" << endl;
        }
        else if (strcmp(buf, "/help") == 0) {
            cout << "\nCommands:" << endl;
            cout << "  /help               - this help" << endl;
            cout << "  /list               - list users online" << endl;
            cout << "  /history            - show last msgs" << endl;
            cout << "  /history N          - show last N msgs" << endl;
            cout << "  /quit               - disconnect" << endl;
            cout << "  /w <nick> <msg>     - private msg" << endl;
            cout << "  /ping               - send 10 pings" << endl;
            cout << "  /ping N             - send N pings" << endl;
            cout << "  /netdiag            - show network diag" << endl;
            cout << "> " << flush;
        }
        else if (strncmp(buf, "/ping", 5) == 0) {
            int cnt = 10;
            const char* p = buf + 5;
            while (*p == ' ') p++;
            if (*p) {
                cnt = atoi(p);
                if (cnt <= 0) cnt = 10;
                if (cnt > 100) {
                    cout << "Max 100 pings" << endl;
                    cnt = 100;
                }
            }
            send_pings(cnt);
            cout << "> " << flush;
        }
        else if (strcmp(buf, "/netdiag") == 0) {
            show_diag();
            cout << "> " << flush;
        }
        else if (strcmp(buf, "/list") == 0) {
            Msg req = {};
            req.typ = T_LIST;
            req.id = gen_id();
            req.ts = time(nullptr);
            strncpy(req.from, nick, MAX_NAME_LEN - 1);
            req.len = 1;
            send_msg(fd, req);
        }
        else if (strncmp(buf, "/history", 8) == 0) {
            Msg req = {};
            req.typ = T_HIST;
            req.id = gen_id();
            req.ts = time(nullptr);
            strncpy(req.from, nick, MAX_NAME_LEN - 1);
            
            const char* p = buf + 8;
            while (*p == ' ') p++;
            if (*p) {
                int n = atoi(p);
                if (n > 0) snprintf(req.data, MAX_DATA_LEN, "%d", n);
            }
            req.len = strlen(req.data) + 1;
            send_msg(fd, req);
        }
        else if (strncmp(buf, "/w ", 3) == 0) {
            char* space = strchr(buf + 3, ' ');
            if (space) {
                *space = '\0';
                char* target = buf + 3;
                char* msg = space + 1;
                
                Msg priv = {};
                priv.typ = T_PRIVATE;
                priv.id = gen_id();
                priv.ts = time(nullptr);
                strncpy(priv.from, nick, MAX_NAME_LEN - 1);
                strncpy(priv.to, target, MAX_NAME_LEN - 1);
                strncpy(priv.data, msg, MAX_DATA_LEN - 1);
                priv.len = strlen(priv.data) + 1;
                
                send_msg(fd, priv);
                add_pend(priv);
                cout << "[Transport][RETRY] send T_PRIVATE (id=" << priv.id << ")" << endl;
            } else {
                cout << "Usage: /w <nick> <msg>" << endl;
            }
            cout << "> " << flush;
        }
        else if (strlen(buf) > 0 && buf[0] != '/') {
            Msg txt = {};
            txt.typ = T_TEXT;
            txt.id = gen_id();
            txt.ts = time(nullptr);
            strncpy(txt.from, nick, MAX_NAME_LEN - 1);
            strncpy(txt.data, buf, MAX_DATA_LEN - 1);
            txt.len = strlen(txt.data) + 1;
            
            send_msg(fd, txt);
            add_pend(txt);
            cout << "[Transport][RETRY] send T_TEXT (id=" << txt.id << ")" << endl;
            cout << "> " << flush;
        }
        else {
            cout << "> " << flush;
        }
    }
    
    retrans_run = false;
    pthread_join(retrans_thr, nullptr);
    
    return 0;
}
