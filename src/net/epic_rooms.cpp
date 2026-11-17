#include "stv/net/epic_rooms.hpp"
#include "stv/base.hpp"
#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_p2p.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <thread>
#include <fstream>
#include <map>

namespace stv::net {
namespace {
constexpr char bucket[] = "SmashTV-L8-v1";
constexpr u8 discovery = 0, control = 1, gameplay = 2;
using Packet = std::vector<u8>;
struct Runtime {
    EOS_EResult result;
    Runtime() {
        EOS_InitializeOptions o{EOS_INITIALIZE_API_LATEST};
        o.ProductName = "Smash TV"; o.ProductVersion = "1";
        result = EOS_Initialize(&o);
    }
    ~Runtime() { if (result == EOS_EResult::EOS_Success) EOS_Shutdown(); }
};
bool success(EOS_EResult r) { return r == EOS_EResult::EOS_Success; }
struct Route {
    EOS_ProductUserId user{};
    uint64_t seen{}, probe{};
    bool listed{};
};
}
struct EpicRooms::Impl {
    std::unique_ptr<Runtime> runtime;
    EOS_HPlatform platform{};
    EOS_HConnect connect{};
    EOS_HLobby directory{};
    EOS_HP2P p2p{};
    EOS_ProductUserId local{};
    EOS_P2P_SocketId socket{EOS_P2P_SOCKETID_API_LATEST, "SmashTV1"};
    EOS_HLobbySearch search{};
    EOS_NotificationId incoming{}, expiration{};
    bool active{}, host{}, pending{}, failed{}, authenticating{};
    std::string name, lobby_id, message;
    uint64_t now{}, next_search{}, next_probe{}, next_id{1};
    std::map<uint32_t, Route> routes;

    ~Impl() {
        if (incoming) EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p, incoming);
        if (expiration) EOS_Connect_RemoveNotifyAuthExpiration(connect, expiration);
        if (search) EOS_LobbySearch_Release(search);
        if (platform) EOS_Platform_Release(platform);
    }
    void error(const char* step, EOS_EResult result) {
        message = std::string(step) + ": " + EOS_EResult_ToString(result);
        failed = true;
    }
    bool initialize(const std::filesystem::path& path, std::string& err) {
        if (platform) return true;
        std::ifstream file(path);
        std::map<std::string,std::string> config;
        for (std::string line; std::getline(file,line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto split = line.find('=');
            if (split != std::string::npos) config[line.substr(0,split)] = line.substr(split+1);
        }
        for (auto key : {"ProductId","SandboxId","DeploymentId","ClientId","ClientSecret"}) {
            if (config[key].empty()) { err = std::string("Set ") + key + " in eos-client.ini beside SmashTV.exe."; return false; }
        }
        if (!runtime) runtime = std::make_unique<Runtime>();
        if (!success(runtime->result)) { err = EOS_EResult_ToString(runtime->result); return false; }
        EOS_Platform_Options o{EOS_PLATFORM_OPTIONS_API_LATEST};
        o.ProductId = config["ProductId"].c_str(); o.SandboxId = config["SandboxId"].c_str();
        o.DeploymentId = config["DeploymentId"].c_str();
        o.ClientCredentials = {config["ClientId"].c_str(), config["ClientSecret"].c_str()};
        o.Flags = EOS_PF_DISABLE_OVERLAY; o.TickBudgetInMilliseconds = 2;
        platform = EOS_Platform_Create(&o);
        if (!platform) { err = "EOS could not open this deployment. Check eos-client.ini."; return false; }
        connect = EOS_Platform_GetConnectInterface(platform);
        directory = EOS_Platform_GetLobbyInterface(platform);
        p2p = EOS_Platform_GetP2PInterface(platform);
        return true;
    }
    void signed_in(EOS_ProductUserId user) {
        authenticating = false; local = user; message.clear();
        if (incoming) return;
        EOS_P2P_AddNotifyPeerConnectionRequestOptions o{EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST,local,&socket};
        incoming = EOS_P2P_AddNotifyPeerConnectionRequest(p2p,&o,this,[](const EOS_P2P_OnIncomingConnectionRequestInfo* d){
            auto& s = *static_cast<Impl*>(d->ClientData);
            if (!s.active || !s.host || s.routes.size() >= 512) return;
            EOS_P2P_AcceptConnectionOptions a{EOS_P2P_ACCEPTCONNECTION_API_LATEST,s.local,d->RemoteUserId,&s.socket};
            EOS_P2P_AcceptConnection(s.p2p,&a);
        });
        EOS_Connect_AddNotifyAuthExpirationOptions exp{EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST};
        expiration = EOS_Connect_AddNotifyAuthExpiration(connect,&exp,this,[](const EOS_Connect_AuthExpirationCallbackInfo* d){
            static_cast<Impl*>(d->ClientData)->login();
        });
    }
    void login() {
        authenticating = true;
        EOS_Connect_Credentials creds{EOS_CONNECT_CREDENTIALS_API_LATEST,nullptr,EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN};
        EOS_Connect_UserLoginInfo info{EOS_CONNECT_USERLOGININFO_API_LATEST,name.c_str()};
        EOS_Connect_LoginOptions o{EOS_CONNECT_LOGIN_API_LATEST,&creds,&info};
        EOS_Connect_Login(connect,&o,this,[](const EOS_Connect_LoginCallbackInfo* d){
            auto& s = *static_cast<Impl*>(d->ClientData);
            if (success(d->ResultCode)) { s.signed_in(d->LocalUserId); return; }
            if (d->ResultCode == EOS_EResult::EOS_InvalidUser && d->ContinuanceToken) {
                EOS_Connect_CreateUserOptions cu{EOS_CONNECT_CREATEUSER_API_LATEST,d->ContinuanceToken};
                EOS_Connect_CreateUser(s.connect,&cu,&s,[](const EOS_Connect_CreateUserCallbackInfo* u){
                    auto& owner = *static_cast<Impl*>(u->ClientData);
                    owner.authenticating = false;
                    if (success(u->ResultCode)) owner.signed_in(u->LocalUserId);
                    else owner.error("Create player",u->ResultCode);
                });
            } else { s.authenticating = false; s.local = nullptr; s.error("EOS login",d->ResultCode); }
        });
    }
    void device_login() {
        authenticating = true; message = "Connecting to Epic Online Services...";
        EOS_Connect_CreateDeviceIdOptions o{EOS_CONNECT_CREATEDEVICEID_API_LATEST,"Windows PC"};
        EOS_Connect_CreateDeviceId(connect,&o,this,[](const EOS_Connect_CreateDeviceIdCallbackInfo* d){
            auto& s = *static_cast<Impl*>(d->ClientData);
            if (success(d->ResultCode) || d->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed) s.login();
            else { s.authenticating = false; s.error("Device sign-in",d->ResultCode); }
        });
    }
    uint32_t route(EOS_ProductUserId user) {
        for (const auto& [id,r] : routes) if (r.user == user) return id;
        if (routes.size() >= 512 || next_id >= 0xffffffffu) return 0;
        auto id = uint32_t(next_id++); routes[id] = {user,now}; return id;
    }
    void update_directory() {
        if (pending || !local || failed) return;
        if ((!active || !host) && !lobby_id.empty()) {
            EOS_Lobby_DestroyLobbyOptions o{EOS_LOBBY_DESTROYLOBBY_API_LATEST,local,lobby_id.c_str()};
            pending = true;
            EOS_Lobby_DestroyLobby(directory,&o,this,[](const EOS_Lobby_DestroyLobbyCallbackInfo* d){
                auto& s = *static_cast<Impl*>(d->ClientData);
                s.pending = false;
                if (success(d->ResultCode) || d->ResultCode == EOS_EResult::EOS_NotFound) { s.lobby_id.clear(); s.message = "Public room closed"; }
                else s.error("Close public listing", d->ResultCode);
            });
        } else if (active && host && lobby_id.empty()) {
            EOS_Lobby_CreateLobbyOptions o{EOS_LOBBY_CREATELOBBY_API_LATEST};
            o.LocalUserId = local; o.MaxLobbyMembers = 2;
            o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
            o.BucketId = bucket; o.bDisableHostMigration = EOS_TRUE;
            pending = true; message = "Publishing room...";
            EOS_Lobby_CreateLobby(directory,&o,this,[](const EOS_Lobby_CreateLobbyCallbackInfo* d){
                auto& s = *static_cast<Impl*>(d->ClientData); s.pending = false;
                if (success(d->ResultCode)) { s.lobby_id = d->LobbyId; s.message = "Public room online"; }
                else s.error("Publish public room",d->ResultCode);
            });
        }
    }
    void find_hosts() {
        next_search = now + 5000;
        EOS_Lobby_CreateLobbySearchOptions create{EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST,EOS_LOBBY_MAX_SEARCH_RESULTS};
        auto result = EOS_Lobby_CreateLobbySearch(directory,&create,&search);
        if (!success(result)) { error("Create search",result); return; }
        EOS_Lobby_AttributeData key{EOS_LOBBY_ATTRIBUTEDATA_API_LATEST};
        key.Key = EOS_LOBBY_SEARCH_BUCKET_ID; key.Value.AsUtf8 = bucket;
        key.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbySearch_SetParameterOptions param{EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST,&key,EOS_EComparisonOp::EOS_CO_EQUAL};
        result = EOS_LobbySearch_SetParameter(search,&param);
        if (!success(result)) { EOS_LobbySearch_Release(search); search = nullptr; error("Filter public rooms",result); return; }
        EOS_LobbySearch_FindOptions find{EOS_LOBBYSEARCH_FIND_API_LATEST,local};
        EOS_LobbySearch_Find(search,&find,this,[](const EOS_LobbySearch_FindCallbackInfo* d){
            auto& s = *static_cast<Impl*>(d->ClientData);
            if (s.active && !s.host && success(d->ResultCode)) {
                for (auto& item : s.routes) item.second.listed = false;
                EOS_LobbySearch_GetSearchResultCountOptions count{EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST};
                auto n = EOS_LobbySearch_GetSearchResultCount(s.search,&count);
                for (uint32_t i=0; i<n; ++i) {
                    EOS_HLobbyDetails details{};
                    EOS_LobbySearch_CopySearchResultByIndexOptions copy{EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST,i};
                    if (!success(EOS_LobbySearch_CopySearchResultByIndex(s.search,&copy,&details))) continue;
                    EOS_LobbyDetails_GetLobbyOwnerOptions owner{EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST};
                    auto user = EOS_LobbyDetails_GetLobbyOwner(details,&owner);
                    EOS_LobbyDetails_Release(details);
                    if (user && user != s.local) {
                        const auto id = s.route(user);
                        if (id) { s.routes[id].listed = true; s.routes[id].seen = s.now; }
                    }
                }
                s.message = n ? "Searching for reachable hosts..." : "No public games found";
            } else if (s.active && !s.host && !success(d->ResultCode)) {
                s.message = std::string("Room search: ") + EOS_EResult_ToString(d->ResultCode);
            }
            EOS_LobbySearch_Release(s.search); s.search = nullptr;
        });
    }
    void send(EOS_ProductUserId to, u8 channel, const Packet& bytes) {
        if (!to || bytes.empty() || bytes.size() > EOS_P2P_MAX_PACKET_SIZE) return;
        EOS_P2P_SendPacketOptions o{EOS_P2P_SENDPACKET_API_LATEST,local,to,&socket,channel,
            static_cast<uint32_t>(bytes.size()),bytes.data(),channel==gameplay ? EOS_FALSE : EOS_TRUE,EOS_EPacketReliability::EOS_PR_UnreliableUnordered};
        EOS_P2P_SendPacket(p2p,&o);
    }
    template<class F> void drain(u8 channel, F consume) {
        EOS_P2P_ReceivePacketOptions o{EOS_P2P_RECEIVEPACKET_API_LATEST,local,EOS_P2P_MAX_PACKET_SIZE,&channel};
        for (unsigned i=0; i<256; ++i) {
            std::array<u8,EOS_P2P_MAX_PACKET_SIZE> bytes;
            EOS_ProductUserId from{}; EOS_P2P_SocketId source{}; uint8_t received{}; uint32_t size{};
            if (!success(EOS_P2P_ReceivePacket(p2p,&o,&from,&source,&received,bytes.data(),&size))) break;
            if (std::strcmp(source.SocketName,socket.SocketName)==0) consume(from,Packet(bytes.begin(),bytes.begin()+size));
        }
    }
    void pump(Lobby& room) {
        const bool browsing = room.state() == LobbyState::Searching;
        if (browsing && !search && now >= next_search) find_hosts();
        if (browsing && now >= next_probe) {
            next_probe = now + 900;
            for (auto& [id,r] : routes) if (r.listed) {
                Packet probe(8); r.probe = now; put64(probe.data(),now);
                send(r.user,discovery,probe);
            }
        }
        drain(discovery,[&](auto user, const Packet& packet){
            if (host && packet.size()==8) {
                auto id = route(user); if (!id) return;
                routes[id].seen = now;
                send(user,discovery,packet);
                room.reply_to_probe({id,1});
            } else if (browsing && packet.size()==8) {
                for (auto& [id,r] : routes) if (r.user == user && r.listed && le64(packet.data()) == r.probe)
                    room.measured_ping({id,1}, int(now-r.probe));
            }
        });
        drain(control,[&](auto user, const Packet& packet){
            auto it = std::find_if(routes.begin(),routes.end(),[&](const auto& item){return item.second.user==user;});
            if (it == routes.end()) return;
            it->second.seen = now;
            room.receive({it->first,1},packet,browsing && it->second.listed,now);
        });
        room.tick(now);
        for (auto& packet : room.take_outgoing()) {
            auto it = routes.find(packet.to.ip);
            if (packet.to.port==1 && it!=routes.end()) send(it->second.user,control,packet.bytes);
        }
        for (auto it=routes.begin();it!=routes.end();) {
            if (now-it->second.seen > 30000) {
                EOS_P2P_CloseConnectionOptions o{EOS_P2P_CLOSECONNECTION_API_LATEST,local,it->second.user,&socket};
                EOS_P2P_CloseConnection(p2p,&o);
                it = routes.erase(it);
            } else ++it;
        }
    }
    struct Game : Transport {
        Impl& owner;
        EOS_ProductUserId peer;
        std::array<u8,16> session{};
        Game(Impl& s, EOS_ProductUserId p, const LobbyHandoff& h) : owner(s),peer(p) {
            put64(session.data(),h.room_id); put64(session.data()+8,h.session_token);
        }
        void send(const Packet& bytes) override {
            Packet packet(session.begin(),session.end());
            packet.insert(packet.end(),bytes.begin(),bytes.end()); owner.send(peer,gameplay,packet);
        }
        std::vector<Packet> poll() override {
            std::vector<Packet> packets;
            owner.drain(gameplay,[&](auto from,const Packet& p){
                if (from==peer && p.size()>session.size() && std::equal(session.begin(),session.end(),p.begin()))
                    packets.emplace_back(p.begin()+session.size(),p.end());
            });
            return packets;
        }
    };
};
EpicRooms::EpicRooms() : impl_(std::make_unique<Impl>()) {}
EpicRooms::~EpicRooms() = default;
bool EpicRooms::open(const std::filesystem::path& path,const std::string& name,bool host,std::string& error) {
    auto& s = *impl_;
    if (!s.initialize(path,error)) return false;
    s.name=name; s.host=host; s.active=true; s.failed=false; s.next_search=s.next_probe=0;
    s.message.clear();
    if (!s.local && !s.authenticating) s.device_login();
    return true;
}
void EpicRooms::close() {
    auto& s=*impl_;
    if (!s.active) return;
    s.active=false;
    if (s.local) {
        EOS_P2P_CloseConnectionsOptions o{EOS_P2P_CLOSECONNECTIONS_API_LATEST,s.local,&s.socket};
        EOS_P2P_CloseConnections(s.p2p,&o);
    }
    s.routes.clear(); s.update_directory();
}
void EpicRooms::tick(Lobby* room,uint64_t now) {
    auto& s=*impl_;
    if (!s.platform) return;
    s.now=now; EOS_Platform_Tick(s.platform); s.update_directory();
    if (s.active && room && s.local && !s.failed) s.pump(*room);
}
void EpicRooms::flush() {
    if (impl_->platform && !impl_->failed) EOS_Platform_Tick(impl_->platform);
}
void EpicRooms::finish() {
    close();
    auto& s=*impl_;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (s.platform && !s.failed && (s.pending || !s.lobby_id.empty()) && std::chrono::steady_clock::now()<deadline) {
        EOS_Platform_Tick(s.platform);
        s.update_directory();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
bool EpicRooms::published() const { return impl_->active && impl_->host && !impl_->pending && !impl_->lobby_id.empty() && !impl_->failed; }
const std::string& EpicRooms::status() const { return impl_->message; }
std::unique_ptr<Transport> EpicRooms::transport(const LobbyHandoff& h) {
    auto it=impl_->routes.find(h.control.ip);
    if (it==impl_->routes.end()) return {};
    return std::make_unique<Impl::Game>(*impl_,it->second.user,h);
}
}
