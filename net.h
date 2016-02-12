#ifndef NET_H
#define NET_H

#include <cstdint>
#include <string.h>

struct Addr {
    uint8_t addr[17];
    bool operator==(const Addr& rhs) {
        if (addr[0] != rhs.addr[0]) {
            return false;
        }
        return memcmp(addr+1, rhs.addr+1, addr[0] ? 16 : 4);
    }
};

struct __attribute__((__packed__)) Msg {
    uint8_t op;
    uint8_t n;
    uint64_t key;
    Addr value;
};


int recvmsg(const Addr& value, Msg& msg);
int sendmsg(const Addr& value, const Msg& msg);


#endif // NET_H
