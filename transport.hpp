#pragma once

#include <kj/io.h>
#include <string_view>

struct Connection {
    kj::AutoCloseFd input;
    kj::AutoCloseFd output;
};

class RoomListener {
public:
    explicit RoomListener(std::string_view room);
    ~RoomListener() noexcept(false);

    kj::AutoCloseFd takeRequests();
    Connection accept(std::string_view request);

private:
    kj::AutoCloseFd directory;
    kj::AutoCloseFd requests;
};

Connection connectToRoom(std::string_view room);
