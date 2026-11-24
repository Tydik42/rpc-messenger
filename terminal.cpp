#include "terminal.hpp"
#include "chat.hpp"

#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <kj/debug.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

bool validNickname(std::string_view nick)
{
    return !nick.empty() && nick.size() <= 64 &&
           std::ranges::all_of(nick, [](unsigned char ch) {
               return ch >= ' ' && ch != 127 && ch != '[' && ch != ']';
           });
}

void Terminal::message(std::string_view nick, std::string_view text, bool own) const
{
    if (color) {
        std::cout << (own ? "\033[32m" : "\033[36m");
    }
    std::cout << '[' << nick << ']';
    if (color) {
        std::cout << "\033[0m";
    }
    std::cout << ' ';
    for (unsigned char ch : text) {
        std::cout.put((ch < ' ' && ch != '\t') || ch == 127 ? '?' : static_cast<char>(ch));
    }
    std::cout << '\n' << std::flush;
}

void Terminal::status(std::string_view text) const
{
    std::cout << "[чат] " << text << '\n' << std::flush;
}

LineReader::LineReader(kj::Own<kj::AsyncInputStream> stream) : stream(kj::mv(stream)) {}

kj::Promise<std::optional<std::string>> LineReader::next()
{
    if (const auto newline = pending.find('\n'); newline != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        return std::optional{std::move(line)};
    }
    if (eof) {
        if (pending.empty()) {
            return std::optional<std::string>{};
        }
        return std::optional{std::exchange(pending, {})};
    }
    if (stream.get() == nullptr) {
        return kj::evalLater([this] {
            ssize_t size;
            KJ_SYSCALL(size = read(STDIN_FILENO, buffer.data(), buffer.size()));
            return consume(static_cast<std::size_t>(size));
        });
    }
    return stream->tryRead(buffer.data(), 1, buffer.size())
        .then([this](std::size_t size) { return consume(size); });
}

kj::Promise<std::optional<std::string>> LineReader::consume(std::size_t size)
{
    eof = size == 0;
    pending.append(buffer.data(), size);
    return next();
}

StdinFlags::StdinFlags() : flags(fcntl(STDIN_FILENO, F_GETFL)) {}

StdinFlags::~StdinFlags()
{
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags);
    }
}

kj::Own<kj::AsyncInputStream> watchStdin(kj::LowLevelAsyncIoProvider& provider)
{
    struct stat info {};
    KJ_SYSCALL(fstat(STDIN_FILENO, &info));
    // Regular files and /dev/null do not need an epoll watcher on Linux.
    if (S_ISREG(info.st_mode) || (S_ISCHR(info.st_mode) && !isatty(STDIN_FILENO))) {
        return {};
    }
    return provider.wrapInputFd(STDIN_FILENO);
}

kj::Promise<void> readChat(LineReader& lines, SendLine& send)
{
    return lines.next().then([&](std::optional<std::string> line) -> kj::Promise<void> {
        if (!line || *line == "/quit") {
            return kj::READY_NOW;
        }
        if (line->empty()) {
            return readChat(lines, send);
        }
        return send(std::move(*line)).then([&] { return readChat(lines, send); });
    });
}
