#include "transport.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr auto requestsName = "join";
constexpr auto toHost = "to-host";
constexpr auto toClient = "to-client";

int checked(int result, const char* operation)
{
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
    return result;
}

std::filesystem::path roomPath(std::string_view room)
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string suffix(room);
    if (runtime && runtime[0] == '/' && access(runtime, W_OK | X_OK) == 0) {
        return std::filesystem::path(runtime) / ("rpc-messenger-" + suffix);
    }

    const char* tmp = std::getenv("TMPDIR");
    return std::filesystem::path(tmp && tmp[0] == '/' ? tmp : "/tmp") /
           ("rpc-messenger-" + std::to_string(geteuid()) + "-" + suffix);
}

kj::AutoCloseFd openRoomDirectory(const std::filesystem::path& path, bool create)
{
    if (create && mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) {
        checked(-1, "Не удалось создать каталог комнаты");
    }

    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (!create && fd < 0 && errno == ENOENT) {
        throw std::runtime_error("Комната не открыта. Сначала запустите listen.");
    }
    kj::AutoCloseFd directory(checked(fd, "Не удалось открыть каталог комнаты"));
    struct stat info {};
    checked(fstat(directory.get(), &info), "Не удалось проверить каталог комнаты");
    if (info.st_uid != geteuid() || (info.st_mode & 0777) != 0700) {
        throw std::runtime_error("Каталог комнаты должен принадлежать текущему пользователю и иметь права 0700.");
    }
    return directory;
}

kj::AutoCloseFd openFifo(int directory, const char* name, int mode)
{
    const int fd = openat(directory, name, mode | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 && std::string_view(name) == requestsName &&
        (errno == ENOENT || errno == ENXIO)) {
        throw std::runtime_error("Комната не открыта. Сначала запустите listen.");
    }
    kj::AutoCloseFd fifo(checked(fd, "Не удалось открыть канал"));
    struct stat info {};
    checked(fstat(fifo.get(), &info), "Не удалось проверить канал");
    if (!S_ISFIFO(info.st_mode)) {
        throw std::runtime_error("Ожидался именованный канал.");
    }
    return fifo;
}

struct ClientDirectory {
    std::string path;
    kj::AutoCloseFd directory;

    ~ClientDirectory() noexcept(false)
    {
        if (directory.get() >= 0) {
            unlinkat(directory.get(), toHost, 0);
            unlinkat(directory.get(), toClient, 0);
        }
        rmdir(path.c_str());
    }
};

void writeRequest(int fd, std::string_view bytes)
{
    const auto count = write(fd, bytes.data(), bytes.size());
    if (count < 0) {
        throw std::system_error(errno, std::generic_category(), "Не удалось передать запрос подключения");
    }
    if (static_cast<std::size_t>(count) != bytes.size()) {
        throw std::runtime_error("Неполный запрос подключения.");
    }
}

void waitForHost(int fd)
{
    pollfd pending{fd, POLLIN, 0};
    if (checked(poll(&pending, 1, 5000), "Ошибка ожидания ответа") == 0) {
        throw std::runtime_error("Комната не ответила на запрос подключения.");
    }
    char reply;
    if (read(fd, &reply, 1) != 1 || reply != '+') {
        throw std::runtime_error("Комната не подтвердила подключение.");
    }
}

}  // namespace

RoomListener::RoomListener(std::string_view room)
    : directory(openRoomDirectory(roomPath(room), true))
{
    checked(flock(directory.get(), LOCK_EX | LOCK_NB), "Комната уже открыта");
    unlinkat(directory.get(), requestsName, 0);
    checked(mkfifoat(directory.get(), requestsName, 0600), "Не удалось создать канал подключения");
    requests = openFifo(directory.get(), requestsName, O_RDWR);
}

RoomListener::~RoomListener() noexcept(false)
{
    unlinkat(directory.get(), requestsName, 0);
}

kj::AutoCloseFd RoomListener::takeRequests()
{
    return kj::mv(requests);
}

Connection RoomListener::accept(std::string_view request)
{
    constexpr std::string_view letters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (request.size() != 13 || !request.starts_with("client-") ||
        request.substr(7).find_first_not_of(letters) != std::string_view::npos) {
        throw std::runtime_error("Некорректный запрос подключения.");
    }
    const std::string name(request);
    kj::AutoCloseFd client(checked(openat(directory.get(), name.c_str(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
                                   "Не удалось открыть каталог подключения"));
    Connection connection{openFifo(client.get(), toHost, O_RDONLY),
                          openFifo(client.get(), toClient, O_WRONLY)};
    writeRequest(connection.output.get(), "+");
    return connection;
}

Connection connectToRoom(std::string_view room)
{
    const auto path = roomPath(room);
    auto directory = openRoomDirectory(path, false);
    auto requests = openFifo(directory.get(), requestsName, O_WRONLY);
    std::string pattern = (path / "client-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        checked(-1, "Не удалось создать каталог подключения");
    }
    ClientDirectory client{std::move(pattern), {}};
    client.directory = openRoomDirectory(client.path, false);
    const int fd = client.directory.get();
    checked(mkfifoat(fd, toHost, 0600), "Не удалось создать канал отправки");
    checked(mkfifoat(fd, toClient, 0600), "Не удалось создать канал приёма");

    // These extra ends keep the FIFOs open until the host confirms its descriptors.
    auto hostReader = openFifo(fd, toHost, O_RDONLY);
    Connection connection{openFifo(fd, toClient, O_RDONLY),
                          openFifo(fd, toHost, O_WRONLY)};
    auto hostWriter = openFifo(fd, toClient, O_WRONLY);
    writeRequest(requests.get(), std::filesystem::path(client.path).filename().string() + "\n");
    waitForHost(connection.input.get());
    return connection;
}
