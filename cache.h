#ifndef CACHE_H
#define CACHE_H

#include <atomic>
#include <mutex>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bits.h"
#include "net.h"
#include "err.h"

constexpr uint8_t Ok        = 0;
constexpr uint8_t None      = 1;
constexpr uint8_t Broken    = 2;
constexpr uint8_t Self      = 3;

enum class SearchErr : uint8_t {
    Ok      = Ok,
    None    = None,
    Self    = Self,
    Broken  = Broken,
};
enum class GetErr : uint8_t {
    Ok      = Ok,
    None    = None,
    Broken  = Broken,
};

enum Op : uint8_t {
    Get     = 0,
    Probe   = 1,
    Pong    = 2,
    Conn    = 3,
    Close   = 4,
};

struct Ele {
    using Key = uint64_t;
    using Val = Addr;
    using Data = struct { Key key; Val value; };
    std::atomic<Key> key;
    Val value;
};

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

    bool compare(const Ele::Key f, const Ele::Key s);
    bool insert(const Ele::Key key, const Ele::Val& value);
    bool remove(const Ele::Key key);
    bool remove(const Ele::Key key, const Ele::Val& value);
    bool remove(const Ele::Data& ele);
    Option<Ele::Data> front();
};

struct Cache {
    Ele::Key id;
    Ele buf[64/2*(64+1)];
    Line lines[64];

    Cache(Ele::Key id);

    int line(Ele::Key key);
    Result<SearchErr, Ele::Val> search(Ele::Key key);
    bool insert(const Ele::Key key, const Ele::Val& value);
    bool remove(const Ele::Key key, const Ele::Val& value);
    int conn(const Ele::Val& value);
    Result<GetErr, Ele::Data> get(const Ele::Data& ele, const unsigned n);

    void process(const Ele::Val& value, const Msg& msg);
    unsigned bootstrap();
    void probe(Line& line);
    void refresh();

};

unsigned partition(Line lines[64], Ele::Data knowns[64]);

#endif // CACHE_H
