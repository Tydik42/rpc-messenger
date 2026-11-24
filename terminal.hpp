#pragma once

#include <array>
#include <kj/async-io.h>
#include <optional>
#include <string>
#include <string_view>

class Terminal {
public:
    explicit Terminal(bool color) : color(color) {}
    void message(std::string_view nick, std::string_view text, bool own = false) const;
    void status(std::string_view text) const;

private:
    bool color;
};

class LineReader {
public:
    explicit LineReader(kj::Own<kj::AsyncInputStream> stream);
    kj::Promise<std::optional<std::string>> next();

private:
    kj::Promise<std::optional<std::string>> consume(std::size_t size);

    kj::Own<kj::AsyncInputStream> stream;
    std::array<char, 4096> buffer;
    std::string pending;
    bool eof = false;
};

class StdinFlags {
public:
    StdinFlags();
    ~StdinFlags();

    StdinFlags(const StdinFlags&) = delete;
    StdinFlags& operator=(const StdinFlags&) = delete;

private:
    int flags;
};

kj::Own<kj::AsyncInputStream> watchStdin(kj::LowLevelAsyncIoProvider& provider);

using SendLine = kj::Function<kj::Promise<void>(std::string)>;
kj::Promise<void> readChat(LineReader& lines, SendLine& send);
