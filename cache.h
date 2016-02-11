#ifndef CACHE_H
#define CACHE_H

#include <atomic>
#include <mutex>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bits.h"

template<typename T> struct Option {
    uint8_t err = true;
    T data;
};

struct Addr {
    uint8_t addr[17];
};

template<typename Err, typename T> struct Result {
    Err err;
    T data;
};

struct Ele {
    using Key = uint64_t;
    using Val = Addr;
    using Data = struct { Key key; Val value; };
    std::atomic<Key> key;
    Val value;
};

bool operator==(const Ele::Val& f, const Ele::Val& s) {
    if (f.addr[0] != s.addr[0]) {
        return false;
    }
    return memcmp(f.addr+1, s.addr+1, f.addr[0] ? 16 : 4);
}

constexpr uint8_t Ok        = 0;
constexpr uint8_t None      = 1;
constexpr uint8_t Broken    = 2;

enum class SearchErr : uint8_t {
    Ok      = Ok,
    None    = None,
    Self    = 2,
};
enum class GetErr : uint8_t {
    Ok      = Ok,
    None    = None,
    Broken  = 2,
};

constexpr unsigned size(const unsigned pos) {
    return 64 - pos;
}

enum Op : uint8_t {
    Get     = 0,
    Probe   = 1,
    Pong    = 2,
    Conn    = 3,
    Close   = 4,
};

struct __attribute__((__packed__)) Msg {
    Op op;
    uint8_t n;
    Ele::Key key;
    Ele::Val value;
};

int sendmsg(const Ele::Val& value, const Msg& msg) {
    (void)msg;
    (void)value;
    return -1;
}

int recvmsg(const Ele::Val& value, Msg& msg) {
    (void)msg;
    (void)value;
    return -1;
}

struct Line {
    using Guard = std::lock_guard<std::mutex>;
    static constexpr auto relax = std::memory_order_relaxed;
    static constexpr auto consume = std::memory_order_consume;
    static constexpr auto acquire = std::memory_order_acquire;

    static constexpr unsigned backup = 3;
    Ele::Key id;
    Ele* init;
    unsigned size;
    std::atomic<Ele*> curr;
    std::mutex m;

    bool compare(const Ele::Key f, const Ele::Key s) {
        return prefix(id, f) > prefix(id, s);
    }
    bool insert(const Ele::Key key, const Ele::Val& value) {
        Ele* it;
        /* this might miss the oportunity of insertion of better match due to
         * current node failing. relying on backups not failing for delayed
         * self-heal
         */
        if ((it = curr.load(consume)) != nullptr
            && it > init + backup
            && compare(it->key.load(acquire), key)) {
            return false;
        }
        Guard lock(m);
        if ((it = curr.load(relax)) == nullptr) {
            init->key.store(key, relax);
            init->value = value;
            curr.store(init, relax);
            return true;
        }
        if (compare(it->key, key)) {
            return false;
        }
        if (++it == init + size) {
            return false;
        }
        it->key.store(key, relax);
        init->value = value;
        curr.store(it, relax);
        return true;
    }
    bool remove(const Ele::Key key) {
        Ele* it;
        if ((it = curr.load(consume)) == nullptr
            || it->key.load(acquire) != key) {
            return false;
        }
        Guard lock(m);
        if ((it = curr.load(relax)) == nullptr) {
            return false;
        }
        if (it->key.load(relax) == key) {
            if (it == init) {
                curr.store(nullptr, relax);
            } else {
                curr.store(it-1, relax);
            }
            return true;
        }
        return false;
    }
    bool remove(const Ele::Key key, const Ele::Val& value) {
        Ele* it;
        if ((it = curr.load(consume)) == nullptr
            || it->key.load(acquire) != key) {
            return false;
        }
        Guard lock(m);
        if ((it = curr.load(relax)) == nullptr) {
            return false;
        }
        if (it->key.load(relax) == key
            && it->value == value) {
            if (it == init) {
                curr.store(nullptr, relax);
            } else {
                curr.store(it-1, relax);
            }
            return true;
        }
        return false;
    }
    bool remove(const Ele::Data& ele) {
        return remove(ele.key, ele.value);
    }
    Option<Ele::Data> front() {
        Option<Ele::Data> res = {};
        Ele* it;
        if ((it = curr.load(consume)) == nullptr) {
            return res;
        }
        {
            Guard lock(m);
            if ((it = curr.load(relax)) != nullptr) {
                res.data.key = it->key.load(relax);
                res.data.value = it->value;
            }
        }
        res.err = (it == nullptr);
        return res;
    }
};

struct Cache {
    Ele::Key id;
    Ele buf[64/2*(64+1)];
    Line lines[64];
    Cache(Ele::Key id) : id{id} {
        Ele* it = buf;
        for(unsigned i = 0; i < 64; ++i) {
            auto& line = lines[i];
            line.init = it;
            line.size = size(i);
            line.curr = nullptr;
            line.id = id^(1ull<<(63-i));
            it += size(i);
        }
    }
    int line(Ele::Key key) {
        auto res = prefix(key, id);
        return (res == 64) ? -1 : res;
    }
    Option<Ele::Data> request(const Ele::Data& who, const unsigned what) {
        Option<Ele::Data> res = {};
        if (who.key == id) {
            return res;
        }
        insert(who.key, who.value);
        return res = lines[what].front();
    }
    Result<SearchErr, Ele::Val> search(Ele::Key key) {
        if (id == key) {
            return {SearchErr::Self, {}};
        }
        auto ele = lines[line(key)].front();
        if (ele.err) {
            return {SearchErr::None, {}};
        }
        if (ele.data.key == key) {
            return {SearchErr::Ok, ele.data.value};
        }
        Result<GetErr, Ele::Data> resp;
        do {
            resp = get(ele.data, prefix(resp.data.key, key));
            if (resp.err == GetErr::Ok) {
                ele.data = resp.data;
                insert(ele.data.key, ele.data.value);
            } else {
                ele.err = true;
                if (resp.err == GetErr::Broken) {
                    remove(ele.data.key, ele.data.value);
                };
                break;
            }
        } while (64 - prefix(ele.data.key, key));
        if (!ele.err) {
            return {SearchErr::Ok, ele.data.value};
        } else {
            return {SearchErr::None, {}};
        }
    }
    bool insert(const Ele::Key key, const Ele::Val& value) {
        if (key == id) {
            return false;
        }
        return lines[line(key)].insert(key, value);
    }
    bool remove(const Ele::Key key, const Ele::Val& value) {
        if (key == id) {
            return false;
        }
        return lines[line(key)].remove(key, value);
    }
    int conn(const Ele::Val& value) {
        Msg msg = {Conn, Ok, id, {}};
        if (sendmsg(value, msg)) {
            return -1;
        }
        if (recvmsg(value, msg)) {
            return -1;
        }
        if (msg.n != Ok || msg.key == id) {
            return -1;
        }
        insert(msg.key, value);
        return 0;
    }
    Result<GetErr, Ele::Data> get(const Ele::Data& ele, const unsigned n) {
        Result<GetErr, Ele::Data> res;
        Msg msg = {Get, Ok, id, {}};
        if (sendmsg(ele.value, msg)) {
            return {GetErr::Broken, {}};
        }
        if (recvmsg(ele.value, msg)) {
            return {GetErr::Broken, {}};
        }
        if (msg.n != 0) {
            return {GetErr::None, {}};
        }
        if (prefix(ele.key, res.data.key) != n) {
            return {GetErr::Broken, {}};
        }
        return {GetErr::Ok, {msg.key, msg.value}};
    }
    void process(const Ele::Val& value, const Msg& msg) {
        Msg resp;
        switch(msg.op) {
        case Conn:
            SearchErr err;
            if ((err = search(msg.key).err) == SearchErr::None) {
                resp = {Pong, Ok, id, {}};
                if (sendmsg(value, msg) == 0) {
                    insert(msg.key, value);
                }
            } else {
                resp = {Pong, uint8_t(err), {}, {}};
                sendmsg(value, msg);
            }
            break;
        case Get:
            if (msg.n < 64) {
                Option<Ele::Data> ele = lines[msg.n].front();
                if (ele.err) {
                    resp = {Pong, None, {}, {}};
                } else {
                    resp = {Pong, Ok, ele.data.key, ele.data.value};
                }
                if (sendmsg(value, msg) == 0) {
                    insert(msg.key, value);
                }
            } else {
                resp = {Pong, Broken, {}, {}};
                sendmsg(value, msg);
            }
            break;
        case Probe:
            resp = {Pong, Ok, id, {}};
            sendmsg(value, msg);
            break;
        case Close:
            remove(msg.key, value);
            break;
        default: break;
        }
    }
};

#endif // CACHE_H
