#include "chat.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <kj/exception.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {

constexpr auto usage =
    "Использование: messenger listen|connect [--nick НИК] [--room ИМЯ] [--color=auto|always|never]\n";

struct Options {
    bool listen = false;
    std::string_view room = "default";
    std::string_view color = "auto";
    std::string_view nick;
    bool help = false;
};

Options parseOptions(int argc, char** argv)
{
    Options options;
    std::string_view command;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto separator = arg.find('=');
        const auto name = arg.substr(0, separator);
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return options;
        }
        if (name == "--room" || name == "--color" || name == "--nick") {
            std::string_view value;
            if (separator != std::string_view::npos) {
                value = arg.substr(separator + 1);
            } else if (index + 1 < argc) {
                value = argv[++index];
            }
            if (name == "--room") {
                options.room = value;
            } else if (name == "--color") {
                options.color = value;
            } else {
                if (!validNickname(value)) {
                    throw std::invalid_argument("Ник должен содержать от 1 до 64 байт без скобок и управляющих символов.");
                }
                options.nick = value;
            }
        } else if (command.empty() && (arg == "listen" || arg == "connect")) {
            command = arg;
        } else {
            throw std::invalid_argument("Неизвестный аргумент.");
        }
    }

    constexpr std::string_view roomChars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    if (command.empty()) {
        throw std::invalid_argument("Укажите listen или connect.");
    }
    if (options.room.empty() || options.room.size() > 32 ||
        options.room.find_first_not_of(roomChars) != std::string_view::npos) {
        throw std::invalid_argument("Имя комнаты: 1–32 символа A–Z, a–z, 0–9, - или _.");
    }
    if (options.color != "auto" && options.color != "always" && options.color != "never") {
        throw std::invalid_argument("Для --color допустимы auto, always и never.");
    }
    options.listen = command == "listen";
    return options;
}

bool useColor(std::string_view mode)
{
    const char* term = std::getenv("TERM");
    return mode == "always" ||
           (mode == "auto" && isatty(STDOUT_FILENO) &&
            (term == nullptr || std::string_view(term) != "dumb"));
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        if (options.help) {
            std::cout << usage;
            return 0;
        }

        std::signal(SIGPIPE, SIG_IGN);
        runChat(options.room, options.nick, options.listen, useColor(options.color));
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n' << usage;
        return 1;
    } catch (const std::system_error& error) {
        if (error.code().value() == EINTR) {
            return 130;
        }
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const kj::Exception& error) {
        std::cerr << "Ошибка RPC: " << error.getDescription().cStr() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
