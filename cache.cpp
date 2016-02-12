#include "cache.h"

static unsigned size(const unsigned pos) {
    return 64 - pos;
}

bool Line::compare(const Ele::Key f, const Ele::Key s) {
    return prefix(id, f) > prefix(id, s);
}

bool Line::insert(const Ele::Key key, const Ele::Val& value) {
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
    it->value = value;
    curr.store(it, relax);
    return true;
}

bool Line::remove(const Ele::Key key) {
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

bool Line::remove(const Ele::Key key, const Ele::Val& value) {
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

bool Line::remove(const Ele::Data& ele) {
    return remove(ele.key, ele.value);
}

Option<Ele::Data> Line::front() {
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

Cache::Cache(Ele::Key id) : id{id} {
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

int Cache::line(Ele::Key key) {
    auto res = prefix(key, id);
    return (res == 64) ? -1 : res;
}

Option<Ele::Data> Cache::request(const Ele::Data& who, const unsigned what) {
    Option<Ele::Data> res = {};
    if (who.key == id) {
        return res;
    }
    insert(who.key, who.value);
    return res = lines[what].front();
}

Result<SearchErr, Ele::Val> Cache::search(Ele::Key key) {
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

bool Cache::insert(const Ele::Key key, const Ele::Val& value) {
    if (key == id) {
        return false;
    }
    return lines[line(key)].insert(key, value);
}

bool Cache::remove(const Ele::Key key, const Ele::Val& value) {
    if (key == id) {
        return false;
    }
    return lines[line(key)].remove(key, value);
}

int Cache::conn(const Ele::Val& value) {
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

Result<GetErr, Ele::Data> Cache::get(const Ele::Data& ele, const unsigned n) {
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

void Cache::process(const Ele::Val& value, const Msg& msg) {
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

void Cache::refresh() {
    for (unsigned i = 0; i < 64; ++i) {
        search(lines[i].id);
    }
}

