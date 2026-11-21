#include "stv/net/epic_rooms.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

// Run once per OS identity on a shared test deployment. A process always
// cleans up its directory listing before reporting a successful host run.
int main(int argc, char** argv) {
    using namespace stv::net;
    using namespace std::chrono;
    if (argc != 3 || (std::string(argv[1])!="host" && std::string(argv[1])!="guest")) return 2;
    const bool host = std::string(argv[1])=="host";
    LobbyConfig config;
    config.internet = true;
    config.node_id = host ? 0x7712 : 0x7713;
    config.name = host ? "EOS TEST" : "TEST GUEST";
    Lobby room(config);
    EpicRooms epic;
    std::string error, previous;
    if (!epic.open(argv[2],config.name,host,error)) { std::cerr<<error<<std::endl; return 1; }
    if (host) { room.set_password("Room admission test"); room.host(1,0); }
    else room.search(0);
    auto begin = steady_clock::now();
    bool rejected=false, exchanged=false, closing=false;
    size_t replies=0;
    std::unique_ptr<Transport> game;
    for (;;) {
        auto now=duration_cast<milliseconds>(steady_clock::now()-begin).count();
        epic.tick(&room,now);
        if (previous!=epic.status()) { previous=epic.status(); std::cout<<previous<<std::endl; }
        if (now>90000 || previous.find("EOS_")!=std::string::npos) break;
        if (host && room.can_start()) room.start(now);
        if (!host && !exchanged && room.state()==LobbyState::Idle) {
            if (room.problem()!="Incorrect room password") {std::cerr<<room.problem()<<std::endl; break;}
            rejected=true; std::cout<<"Wrong password rejected"<<std::endl; room.search(now);
        }
        if (!host && room.state()==LobbyState::Searching) {
            for (auto& entry : room.rooms()) if (entry.host_id==0x7712 && entry.locked && entry.ping_ms>=0) {
                std::cout<<"Measured host RTT "<<entry.ping_ms<<" ms"<<std::endl;
                room.set_password(rejected ? "Room admission test" : "wrong");
                room.join(entry,1,now); break;
            }
        }
        if (auto handoff=room.take_handoff()) {
            game=epic.transport(*handoff);
            if (!game || !room.established(handoff->session_token,now)) break;
            std::cout<<"Gameplay handoff"<<std::endl;
        }
        if (game && room.state()==LobbyState::Playing) {
            const std::vector<uint8_t> check(1040,host ? 0x12 : 0x34);
            game->send(check);
            for (auto& packet : game->poll()) {
                if (packet!=std::vector<uint8_t>(1040,host ? 0x34 : 0x12)) return 1;
                ++replies;
            }
            if (replies>=5) exchanged=true;
            if (!host && exchanged) {
                room.leave(now); epic.tick(&room,now);
                std::cout<<"Bidirectional gameplay packets; guest left"<<std::endl;
                return 0;
            }
        }
        if (host && exchanged && room.state()==LobbyState::Idle && !closing) {
            std::cout<<"Guest departure observed"<<std::endl;
            epic.close(); closing=true;
        }
        if (closing && previous=="Public room closed") return 0;
        std::this_thread::sleep_for(milliseconds(10));
    }
    epic.close(); return 1;
}
