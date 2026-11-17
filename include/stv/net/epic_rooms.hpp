#pragma once
#include "stv/net/lobby.hpp"
#include "stv/net/rollback.hpp"
#include <filesystem>

namespace stv::net {
// The directory advertises hosts. The existing room protocol owns both seats,
// compatibility, admission and handoff. EOS product user IDs identify packet senders.
class EpicRooms {
public:
    EpicRooms();
    ~EpicRooms();
    bool open(const std::filesystem::path& config, const std::string& name, bool host, std::string& error);
    void close();
    void finish();
    bool published() const;
    void tick(Lobby* room, std::uint64_t now);
    void flush();   // put queued gameplay packets on the wire now
    const std::string& status() const;
    std::unique_ptr<Transport> transport(const LobbyHandoff& handoff);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
