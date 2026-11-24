#include "chat.hpp"
#include "chat.capnp.h"
#include "terminal.hpp"
#include "transport.hpp"

#include <algorithm>
#include <capnp/rpc-twoparty.h>
#include <csignal>
#include <iostream>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <map>
#include <optional>
#include <string>

namespace {

class PipeStream final : public kj::AsyncIoStream {
public:
    PipeStream(kj::LowLevelAsyncIoProvider& provider, Connection connection)
        : input(provider.wrapInputFd(kj::mv(connection.input))),
          output(provider.wrapOutputFd(kj::mv(connection.output))) {}

    kj::Promise<std::size_t> tryRead(void* buffer, std::size_t min, std::size_t max) override
    {
        return input->tryRead(buffer, min, max);
    }

    kj::Promise<void> write(const void* buffer, std::size_t size) override
    {
        return output->write(buffer, size);
    }

    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override
    {
        return output->write(pieces);
    }

    kj::Promise<void> whenWriteDisconnected() override { return output->whenWriteDisconnected(); }
    void shutdownWrite() override { output = nullptr; }

private:
    kj::Own<kj::AsyncInputStream> input;
    kj::Own<kj::AsyncOutputStream> output;
};

std::string_view textView(capnp::Text::Reader value)
{
    return {value.begin(), value.size()};
}

class Incoming final : public Participant::Server {
public:
    explicit Incoming(const Terminal& terminal) : terminal(terminal) {}

    kj::Promise<void> showMessage(ShowMessageContext context) override
    {
        const auto message = context.getParams();
        const auto nick = textView(message.getNick());
        terminal.message(validNickname(nick) ? nick : "собеседник", textView(message.getText()));
        return kj::READY_NOW;
    }

private:
    const Terminal& terminal;
};

class Hub final : public kj::TaskSet::ErrorHandler {
public:
    Hub(const Terminal& terminal, std::string_view nick) : terminal(terminal), hostNick(nick) {}

    std::size_t join(std::string nick, Participant::Client participant)
    {
        const std::size_t id = nextId++;
        if (nick.empty()) {
            nick = "участник-" + std::to_string(id);
            while (hasNickname(nick)) {
                nick += '_';
            }
        }
        KJ_REQUIRE(validNickname(nick), "Некорректный ник.");
        KJ_REQUIRE(!hasNickname(nick), "Этот ник уже занят.");
        members.emplace(id, Member{std::move(nick), kj::mv(participant)});
        terminal.status(nickname(id) + " подключился");
        return id;
    }

    const std::string& nickname(std::size_t id) const { return members.at(id).nick; }
    void leave(std::size_t id)
    {
        const auto member = members.find(id);
        if (member != members.end()) {
            terminal.status(member->second.nick + " вышел");
            members.erase(member);
        }
    }

    kj::Promise<void> waitForDeliveries() { return deliveries.onEmpty(); }

    void broadcast(std::size_t sender, kj::StringPtr text)
    {
        const std::string& nick = sender == 0 ? hostNick : nickname(sender);
        terminal.message(sender == 0 ? "я" : nick, std::string_view(text.begin(), text.size()),
                         sender == 0);
        for (auto& [id, member] : members) {
            if (id != sender) {
                auto request = member.participant.showMessageRequest();
                request.setNick(kj::StringPtr(nick.data(), nick.size()));
                request.setText(text);
                deliveries.add(request.send().ignoreResult());
            }
        }
    }

private:
    struct Member {
        std::string nick;
        Participant::Client participant;
    };

    bool hasNickname(std::string_view nick) const
    {
        return nick == hostNick || std::ranges::any_of(members, [&](const auto& member) {
            return nick == member.second.nick;
        });
    }

    void taskFailed(kj::Exception&& error) override
    {
        if (error.getType() != kj::Exception::Type::DISCONNECTED) {
            std::cerr << "Ошибка RPC: " << error.getDescription().cStr() << '\n';
        }
    }

    const Terminal& terminal;
    std::string hostNick;
    std::size_t nextId = 1;
    std::map<std::size_t, Member> members;
    kj::TaskSet deliveries{*this};
};

class Membership final : public Session::Server {
public:
    Membership(Hub& hub, std::size_t id) : hub(hub), id(id) {}
    ~Membership() noexcept(false) { hub.leave(id); }

    kj::Promise<void> sendMessage(SendMessageContext context) override
    {
        hub.broadcast(id, context.getParams().getText());
        return kj::READY_NOW;
    }

private:
    Hub& hub;
    std::size_t id;
};

class RoomService final : public Room::Server {
public:
    explicit RoomService(Hub& hub) : hub(hub) {}

    kj::Promise<void> join(JoinContext context) override
    {
        KJ_REQUIRE(!joined, "Этот терминал уже подключён.");
        auto params = context.getParams();
        const auto id = hub.join(std::string(textView(params.getNick())), params.getParticipant());
        joined = true;
        auto results = context.getResults();
        const auto& nick = hub.nickname(id);
        results.setNick(kj::StringPtr(nick.data(), nick.size()));
        results.setSession(kj::heap<Membership>(hub, id));
        return kj::READY_NOW;
    }

private:
    Hub& hub;
    bool joined = false;
};

struct Peer {
    Peer(kj::LowLevelAsyncIoProvider& provider, Connection connection, Hub& hub)
        : stream(provider, kj::mv(connection)),
          rpc(stream, kj::heap<RoomService>(hub), capnp::rpc::twoparty::Side::SERVER) {}

    PipeStream stream;
    capnp::TwoPartyClient rpc;
};

kj::Promise<void> acceptPeers(LineReader& requests, RoomListener& listener,
                              kj::LowLevelAsyncIoProvider& provider, Hub& hub,
                              kj::TaskSet& peers)
{
    return requests.next().then([&](std::optional<std::string> name) -> kj::Promise<void> {
        if (!name) {
            return kj::READY_NOW;
        }
        try {
            auto peer = kj::heap<Peer>(provider, listener.accept(*name), hub);
            peers.add(peer->rpc.onDisconnect().attach(kj::mv(peer)));
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
        return acceptPeers(requests, listener, provider, hub, peers);
    });
}

void runHost(std::string_view room, std::string_view nick, const Terminal& terminal)
{
    kj::UnixEventPort::captureSignal(SIGINT);
    auto io = kj::setupAsyncIo();
    RoomListener listener(room);
    LineReader requests(io.lowLevelProvider->wrapInputFd(listener.takeRequests()));
    StdinFlags stdinFlags;
    LineReader input(watchStdin(*io.lowLevelProvider));
    Hub hub(terminal, nick.empty() ? "ведущий" : nick);
    kj::TaskSet peers(hub);
    SendLine send = [&](std::string text) -> kj::Promise<void> {
        hub.broadcast(0, kj::StringPtr(text.data(), text.size()));
        return kj::READY_NOW;
    };

    terminal.status("Комната открыта. Запустите connect в других терминалах.");
    readChat(input, send)
        .exclusiveJoin(acceptPeers(requests, listener, *io.lowLevelProvider, hub, peers))
        .exclusiveJoin(io.unixEventPort.onSignal(SIGINT).ignoreResult())
        .then([&] {
            return hub.waitForDeliveries().exclusiveJoin(
                io.provider->getTimer().afterDelay(kj::SECONDS));
        })
        .wait(io.waitScope);
}

void runClient(std::string_view room, std::string_view nick, const Terminal& terminal)
{
    auto connection = connectToRoom(room);
    kj::UnixEventPort::captureSignal(SIGINT);
    auto io = kj::setupAsyncIo();
    PipeStream stream(*io.lowLevelProvider, kj::mv(connection));
    capnp::TwoPartyClient rpc(stream);
    auto service = rpc.bootstrap().castAs<Room>();

    auto join = service.joinRequest();
    if (!nick.empty()) {
        join.setNick(kj::StringPtr(nick.data(), nick.size()));
    }
    join.setParticipant(kj::heap<Incoming>(terminal));
    auto joined = join.send().wait(io.waitScope);
    auto session = joined.getSession();

    StdinFlags stdinFlags;
    LineReader input(watchStdin(*io.lowLevelProvider));
    SendLine send = [&](std::string text) {
        auto request = session.sendMessageRequest();
        request.setText(kj::StringPtr(text.data(), text.size()));
        return request.send().then([&, text = std::move(text)](auto&&) {
            terminal.message("я", text, true);
        });
    };

    terminal.status("Подключение установлено. Ваш ник — " +
                    std::string(textView(joined.getNick())));
    readChat(input, send)
        .exclusiveJoin(rpc.onDisconnect().then([&] { terminal.status("Комната закрыта."); }))
        .exclusiveJoin(io.unixEventPort.onSignal(SIGINT).ignoreResult())
        .wait(io.waitScope);
}

}  // namespace

void runChat(std::string_view room, std::string_view nick, bool listen, bool color)
{
    const Terminal terminal(color);
    if (listen) {
        runHost(room, nick, terminal);
    } else {
        runClient(room, nick, terminal);
    }
}
