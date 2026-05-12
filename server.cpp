#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <map>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <cstdlib>
#include <random>
#include <chrono>
#include "messages.h"

#define PORT 8888
#define THR_POOL_SZ 10
#define HIST_FILE "chat_history.json"
#define MAX_RETRY_IDS 32

using namespace std;

struct Client {
    int fd;
    char ip[INET_ADDRSTRLEN];
    int port;
    char nick[MAX_NAME_LEN];
    bool auth;
    uint32_t last_ids[MAX_RETRY_IDS];
    int last_cnt;
    pthread_mutex_t mtx;
};

queue<int> work_q;
vector<Client*> clients;
map<string, queue<OffMsg>> offline_q;

pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cl_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t hist_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t id_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;

volatile bool srv_run = true;
volatile uint32_t global_id = 1;

int sim_delay = 0;
double sim_drop = 0.0;
double sim_corrupt = 0.0;

random_device rd;
mt19937 rng(rd());
uniform_real_distribution<> rand01(0.0, 1.0);

string esc_json(const char* s) {
    string res;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"': res += "\\\""; break;
            case '\\': res += "\\\\"; break;
            case '\n': res += "\\n"; break;
            case '\r': res += "\\r"; break;
            case '\t': res += "\\t"; break;
            default: res += *p; break;
        }
    }
    return res;
}

string fmt_ts(time_t t) {
    char buf[MAX_TIME_STR_LEN];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return string(buf);
}

uint32_t gen_id() {
    pthread_mutex_lock(&id_mtx);
    uint32_t id = global_id++;
    pthread_mutex_unlock(&id_mtx);
    return id;
}

void log_tr(const char* tag, const char* msg, uint32_t id = 0) {
    if (id > 0) cout << "[Transport][" << tag << "] " << msg << " (id=" << id << ")" << endl;
    else cout << "[Transport][" << tag << "] " << msg << endl;
}

void log_app(const char* msg) {
    cout << "[Application] " << msg << endl;
}

void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--delay=", 8) == 0) {
            sim_delay = atoi(argv[i] + 8);
            cout << "[Transport][SIM] Delay: " << sim_delay << " ms" << endl;
        } else if (strncmp(argv[i], "--drop=", 7) == 0) {
            sim_drop = atof(argv[i] + 7);
            cout << "[Transport][SIM] Drop rate: " << sim_drop * 100 << "%" << endl;
        } else if (strncmp(argv[i], "--corrupt=", 10) == 0) {
            sim_corrupt = atof(argv[i] + 10);
            cout << "[Transport][SIM] Corrupt rate: " << sim_corrupt * 100 << "%" << endl;
        }
    }
}

bool sim_net(int fd, Msg& m) {
    if (sim_delay > 0) {
        log_tr("SIM", "DELAY", m.id);
        usleep(sim_delay * 1000);
    }
    
    if (sim_drop > 0 && rand01(rng) < sim_drop) {
        log_tr("SIM", "DROP", m.id);
        return true;
    }
    
    if (sim_corrupt > 0 && rand01(rng) < sim_corrupt) {
        log_tr("SIM", "CORRUPT", m.id);
        if (m.len > 1) {
            int pos = rand() % (m.len - 1);
            m.data[pos] = m.data[pos] ^ 0xFF;
        }
    }
    return false;
}

bool is_dup(Client* c, uint32_t id) {
    for (int i = 0; i < c->last_cnt; i++) {
        if (c->last_ids[i] == id) return true;
    }
    return false;
}

void add_id(Client* c, uint32_t id) {
    if (c->last_cnt >= MAX_RETRY_IDS) {
        for (int i = 1; i < MAX_RETRY_IDS; i++) {
            c->last_ids[i - 1] = c->last_ids[i];
        }
        c->last_cnt = MAX_RETRY_IDS - 1;
    }
    c->last_ids[c->last_cnt++] = id;
}

void send_ack(int fd, uint32_t id) {
    Msg ack = {};
    ack.typ = T_ACK;
    ack.id = gen_id();
    ack.ts = time(nullptr);
    strncpy(ack.from, "SERVER", MAX_NAME_LEN - 1);
    snprintf(ack.data, MAX_DATA_LEN, "%u", id);
    ack.len = strlen(ack.data) + 1;
    
    send_msg(fd, ack);
    log_tr("ACK", "send T_ACK", id);
}

void append_hist(const Msg& m, bool delivered, bool offline) {
    pthread_mutex_lock(&hist_mtx);
    
    ofstream file(HIST_FILE, ios::app);
    if (!file.is_open()) {
        pthread_mutex_unlock(&hist_mtx);
        return;
    }
    
    const char* typ_str = "UNKNOWN";
    switch(m.typ) {
        case T_TEXT: typ_str = "T_TEXT"; break;
        case T_PRIVATE: typ_str = "T_PRIVATE"; break;
        case T_HELLO: typ_str = "T_HELLO"; break;
        case T_WELCOME: typ_str = "T_WELCOME"; break;
        case T_PING: typ_str = "T_PING"; break;
        case T_PONG: typ_str = "T_PONG"; break;
        case T_BYE: typ_str = "T_BYE"; break;
        case T_AUTH: typ_str = "T_AUTH"; break;
        case T_ERR: typ_str = "T_ERR"; break;
        case T_SRV_INFO: typ_str = "T_SRV_INFO"; break;
        case T_LIST: typ_str = "T_LIST"; break;
        case T_HIST: typ_str = "T_HIST"; break;
        case T_HIST_DATA: typ_str = "T_HIST_DATA"; break;
        case T_ACK: typ_str = "T_ACK"; break;
    }
    
    file << "{\n";
    file << "  \"id\": " << m.id << ",\n";
    file << "  \"ts\": " << m.ts << ",\n";
    file << "  \"from\": \"" << esc_json(m.from) << "\",\n";
    file << "  \"to\": \"" << esc_json(m.to) << "\",\n";
    file << "  \"type\": \"" << typ_str << "\",\n";
    file << "  \"data\": \"" << esc_json(m.data) << "\",\n";
    file << "  \"delivered\": " << (delivered ? "true" : "false") << ",\n";
    file << "  \"offline\": " << (offline ? "true" : "false") << "\n";
    file << "}\n";
    
    file.close();
    pthread_mutex_unlock(&hist_mtx);
}

void send_hist(int fd, int limit = -1) {
    pthread_mutex_lock(&hist_mtx);
    
    ifstream file(HIST_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&hist_mtx);
        return;
    }
    
    string line, block;
    vector<string> msgs;
    int brace = 0;
    
    while (getline(file, line)) {
        if (line.find('{') != string::npos) brace++;
        if (line.find('}') != string::npos) brace--;
        block += line + "\n";
        
        if (brace == 0 && !block.empty()) {
            msgs.push_back(block);
            block.clear();
        }
    }
    file.close();
    
    int start = (limit > 0 && limit < (int)msgs.size()) ? msgs.size() - limit : 0;
    
    for (size_t i = start; i < msgs.size(); ++i) {
        Msg hm = {};
        hm.typ = T_HIST_DATA;
        hm.id = gen_id();
        hm.ts = time(nullptr);
        strncpy(hm.data, msgs[i].c_str(), MAX_DATA_LEN - 1);
        hm.len = strlen(hm.data) + 1;
        send_msg(fd, hm);
    }
    
    pthread_mutex_unlock(&hist_mtx);
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

Client* find_by_nick(const char* nick) {
    pthread_mutex_lock(&cl_mtx);
    for (auto* c : clients) {
        if (c->auth && strcmp(c->nick, nick) == 0) {
            pthread_mutex_unlock(&cl_mtx);
            return c;
        }
    }
    pthread_mutex_unlock(&cl_mtx);
    return nullptr;
}

void store_offline(const char* from, const char* to, const char* data, uint32_t id, time_t ts) {
    OffMsg off;
    strncpy(off.from, from, MAX_NAME_LEN - 1);
    strncpy(off.to, to, MAX_NAME_LEN - 1);
    strncpy(off.txt, data, MAX_DATA_LEN - 1);
    off.id = id;
    off.ts = ts;
    
    pthread_mutex_lock(&cl_mtx);
    offline_q[to].push(off);
    pthread_mutex_unlock(&cl_mtx);
    
    log_app(("stored offline for " + string(to)).c_str());
}

void deliver_offline(Client* c) {
    pthread_mutex_lock(&cl_mtx);
    auto it = offline_q.find(c->nick);
    if (it != offline_q.end() && !it->second.empty()) {
        log_app(("delivering " + to_string(it->second.size()) + " offline msgs to " + c->nick).c_str());
        
        while (!it->second.empty()) {
            OffMsg& off = it->second.front();
            
            Msg m = {};
            m.typ = T_PRIVATE;
            m.id = off.id;
            m.ts = off.ts;
            strncpy(m.from, off.from, MAX_NAME_LEN - 1);
            strncpy(m.to, off.to, MAX_NAME_LEN - 1);
            snprintf(m.data, MAX_DATA_LEN, "[OFFLINE] %s", off.txt);
            m.len = strlen(m.data) + 1;
            
            send_msg(c->fd, m);
            append_hist(m, true, false);
            it->second.pop();
        }
        offline_q.erase(it);
    }
    pthread_mutex_unlock(&cl_mtx);
}

int handle_auth_msg(Client* c, Msg& m) {
    if (is_dup(c, m.id)) {
        log_tr("DEDUP", "duplicate ignored", m.id);
        send_ack(c->fd, m.id);
        return 1;
    }
    
    switch (m.typ) {
        case T_TEXT: {
            log_app(("process T_TEXT id=" + to_string(m.id)).c_str());
            
            pthread_mutex_lock(&cl_mtx);
            for (auto* other : clients) {
                if (other->auth && other->fd != c->fd) {
                    Msg broadcast = m;
                    snprintf(broadcast.data, MAX_DATA_LEN, "[%s]: %s", m.from, m.data);
                    broadcast.len = strlen(broadcast.data) + 1;
                    send_msg(other->fd, broadcast);
                }
            }
            pthread_mutex_unlock(&cl_mtx);
            
            append_hist(m, true, false);
            add_id(c, m.id);
            send_ack(c->fd, m.id);
            break;
        }
        
        case T_PRIVATE: {
            log_app(("process T_PRIVATE id=" + to_string(m.id)).c_str());
            
            Client* target = find_by_nick(m.to);
            if (target) {
                char fmt[MAX_DATA_LEN];
                snprintf(fmt, MAX_DATA_LEN, "[PVT][%s -> %s]: %s", m.from, m.to, m.data);
                
                Msg priv = m;
                strncpy(priv.data, fmt, MAX_DATA_LEN - 1);
                priv.len = strlen(fmt) + 1;
                send_msg(target->fd, priv);
                append_hist(m, true, false);
            } else {
                store_offline(m.from, m.to, m.data, m.id, m.ts);
                append_hist(m, false, true);
                
                char pending[MAX_DATA_LEN];
                snprintf(pending, sizeof(pending), "[SERVER]: %s offline, msg queued", m.to);
                Msg pending_msg = {};
                pending_msg.typ = T_SRV_INFO;
                pending_msg.id = gen_id();
                pending_msg.ts = time(nullptr);
                strncpy(pending_msg.from, "SERVER", MAX_NAME_LEN - 1);
                strncpy(pending_msg.data, pending, MAX_DATA_LEN - 1);
                pending_msg.len = strlen(pending) + 1;
                send_msg(c->fd, pending_msg);
            }
            add_id(c, m.id);
            send_ack(c->fd, m.id);
            break;
        }
        
        case T_PING: {
            log_tr("PING", "recv T_PING", m.id);
            
            Msg pong = {};
            pong.typ = T_PONG;
            pong.id = m.id;
            pong.ts = time(nullptr);
            strncpy(pong.from, "SERVER", MAX_NAME_LEN - 1);
            strncpy(pong.data, "pong", MAX_DATA_LEN - 1);
            pong.len = strlen(pong.data) + 1;
            send_msg(c->fd, pong);
            
            log_tr("PING", "send T_PONG", m.id);
            break;
        }
        
        case T_LIST: {
            pthread_mutex_lock(&cl_mtx);
            string list = "[SERVER]: Online users\n";
            for (const auto* other : clients) {
                if (other->auth) list += string(other->nick) + "\n";
            }
            pthread_mutex_unlock(&cl_mtx);
            
            Msg list_msg = {};
            list_msg.typ = T_SRV_INFO;
            list_msg.id = gen_id();
            list_msg.ts = time(nullptr);
            strncpy(list_msg.from, "SERVER", MAX_NAME_LEN - 1);
            strncpy(list_msg.data, list.c_str(), MAX_DATA_LEN - 1);
            list_msg.len = strlen(list_msg.data) + 1;
            send_msg(c->fd, list_msg);
            break;
        }
        
        case T_HIST: {
            int limit = -1;
            if (m.len > 1 && m.data[0] != '\0') {
                limit = atoi(m.data);
                if (limit <= 0) limit = -1;
            }
            send_hist(c->fd, limit);
            break;
        }
        
        case T_BYE:
            return 0;
            
        default:
            log_app(("unknown type: " + to_string(m.typ)).c_str());
            break;
    }
    return 1;
}

int auth_client(int fd, Client* c) {
    Msg auth = {};
    if (recv_msg(fd, auth) != 0) return -1;
    
    if (auth.typ != T_AUTH) {
        log_tr("AUTH", "failed: expected T_AUTH");
        return -1;
    }
    
    if (strlen(auth.data) == 0 || strlen(auth.data) >= MAX_NAME_LEN) {
        log_tr("AUTH", "failed: invalid nick");
        return -1;
    }
    
    pthread_mutex_lock(&cl_mtx);
    for (const auto* other : clients) {
        if (other->auth && strcmp(other->nick, auth.data) == 0) {
            pthread_mutex_unlock(&cl_mtx);
            log_tr("AUTH", "failed: nick taken");
            return -1;
        }
    }
    pthread_mutex_unlock(&cl_mtx);
    
    strncpy(c->nick, auth.data, MAX_NAME_LEN - 1);
    c->auth = true;
    
    log_app(("auth success: " + string(c->nick)).c_str());
    
    Msg welcome = {};
    welcome.typ = T_WELCOME;
    welcome.id = gen_id();
    welcome.ts = time(nullptr);
    strncpy(welcome.from, "SERVER", MAX_NAME_LEN - 1);
    snprintf(welcome.data, MAX_DATA_LEN, "Welcome, %s! Type /help for commands.", c->nick);
    welcome.len = strlen(welcome.data) + 1;
    send_msg(fd, welcome);
    
    deliver_offline(c);
    
    log_app(("User " + string(c->nick) + " connected").c_str());
    
    return 0;
}

void* worker_proc(void* arg) {
    while (srv_run) {
        int fd = -1;
        
        pthread_mutex_lock(&q_mtx);
        while (work_q.empty() && srv_run) {
            pthread_cond_wait(&q_cond, &q_mtx);
        }
        if (!work_q.empty()) {
            fd = work_q.front();
            work_q.pop();
        }
        pthread_mutex_unlock(&q_mtx);
        
        if (fd == -1) continue;
        
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        getpeername(fd, (struct sockaddr*)&addr, &addr_len);
        
        Client* c = new Client();
        c->fd = fd;
        c->auth = false;
        c->last_cnt = 0;
        pthread_mutex_init(&c->mtx, nullptr);
        inet_ntop(AF_INET, &addr.sin_addr, c->ip, INET_ADDRSTRLEN);
        c->port = ntohs(addr.sin_port);
        
        log_app(("Client connected from " + string(c->ip) + ":" + to_string(c->port)).c_str());
        
        Msg hello = {};
        if (recv_msg(fd, hello) == 0 && hello.typ == T_HELLO) {
            Msg welcome = {};
            welcome.typ = T_WELCOME;
            welcome.id = gen_id();
            welcome.ts = time(nullptr);
            strncpy(welcome.from, "SERVER", MAX_NAME_LEN - 1);
            strncpy(welcome.data, "Server ready. Send T_AUTH with your nick.", MAX_DATA_LEN - 1);
            welcome.len = strlen(welcome.data) + 1;
            send_msg(fd, welcome);
        }
        
        if (auth_client(fd, c) != 0) {
            close(fd);
            delete c;
            continue;
        }
        
        pthread_mutex_lock(&cl_mtx);
        clients.push_back(c);
        pthread_mutex_unlock(&cl_mtx);
        
        bool active = true;
        while (active && srv_run) {
            Msg m = {};
            if (recv_msg(fd, m) != 0) {
                active = false;
                break;
            }
            
            if (sim_net(fd, m)) continue;
            
            active = (handle_auth_msg(c, m) == 1);
        }
        
        pthread_mutex_lock(&cl_mtx);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if ((*it)->fd == fd) {
                log_app(("User " + string((*it)->nick) + " disconnected").c_str());
                pthread_mutex_destroy(&(*it)->mtx);
                delete *it;
                clients.erase(it);
                break;
            }
        }
        pthread_mutex_unlock(&cl_mtx);
        close(fd);
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    parse_args(argc, argv);
    
    int srv_fd;
    struct sockaddr_in srv_addr;
    
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        cerr << "Socket failed" << endl;
        return 1;
    }
    
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port = htons(PORT);
    
    if (bind(srv_fd, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
        cerr << "Bind failed" << endl;
        close(srv_fd);
        return 1;
    }
    
    if (listen(srv_fd, 10) < 0) {
        cerr << "Listen failed" << endl;
        close(srv_fd);
        return 1;
    }
    
    cout << "========================================" << endl;
    cout << "TCP/IP Chat Server v2.0" << endl;
    cout << "Port: " << PORT << endl;
    cout << "Thread pool: " << THR_POOL_SZ << endl;
    cout << "History: " << HIST_FILE << endl;
    cout << "========================================" << endl;
    
    pthread_t thr[THR_POOL_SZ];
    for (int i = 0; i < THR_POOL_SZ; ++i) {
        pthread_create(&thr[i], nullptr, worker_proc, nullptr);
    }
    
    while (srv_run) {
        struct sockaddr_in cl_addr;
        socklen_t cl_len = sizeof(cl_addr);
        int cl_fd = accept(srv_fd, (struct sockaddr*)&cl_addr, &cl_len);
        
        if (cl_fd < 0) continue;
        
        pthread_mutex_lock(&q_mtx);
        work_q.push(cl_fd);
        pthread_cond_signal(&q_cond);
        pthread_mutex_unlock(&q_mtx);
    }
    
    close(srv_fd);
    return 0;
}
