// Recomp global leaderboard client. See rl_leaderboard.hpp.
#include "rl_leaderboard.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 3
#endif

namespace rl::lb {
namespace {

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hex(const unsigned char* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { out.push_back(digits[p[i] >> 4]); out.push_back(digits[p[i] & 15]); }
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

std::vector<std::string> lines_of(const std::string& body) {
    std::vector<std::string> out;
    for (auto& line : split(body, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool to_u64(const std::string& s, std::uint64_t& v) {
    if (s.empty() || s.size() > 19) return false;
    v = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + unsigned(c - '0'); }
    return true;
}

bool to_int(const std::string& s, int& v) {
    std::uint64_t u = 0;
    if (!to_u64(s, u) || u > 0x7fffffffu) return false;
    v = int(u);
    return true;
}

bool read_file(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.clear();
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.append(buffer, n);
    std::fclose(f);
    return true;
}

bool write_file(const std::wstring& path, const std::string& data) {
    const auto temp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") || !f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !ok) { DeleteFileW(temp.c_str()); return false; }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::string bcrypt_hash(const std::string& key, const std::string& msg, bool hmac) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0) return {};
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32]{};
    bool ok = BCryptCreateHash(alg, &h, nullptr, 0,
                               hmac ? PUCHAR(key.data()) : nullptr, hmac ? ULONG(key.size()) : 0, 0) == 0;
    ok = ok && BCryptHashData(h, PUCHAR(msg.data()), ULONG(msg.size()), 0) == 0;
    ok = ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? hex(digest, sizeof digest) : std::string();
}

struct Endpoint {
    std::wstring host, base;
    INTERNET_PORT port = 0;
    bool secure = true;
    bool ok = false;
};

Endpoint parse_url(const std::wstring& url) {
    Endpoint e;
    if (url.empty()) return e;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256]{}, path[512]{};
    parts.lpszHostName = host; parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 511;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return e;
    e.host = host;
    e.base = path;
    while (!e.base.empty() && e.base.back() == L'/') e.base.pop_back();
    e.port = parts.nPort;
    e.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    e.ok = !e.host.empty();
    return e;
}

}  // namespace

std::string sha256_hex(const std::string& s) { return bcrypt_hash({}, s, false); }
std::string hmac_sha256_hex(const std::string& key, const std::string& msg) { return bcrypt_hash(key, msg, true); }
std::string pid_from_install_id(const std::string& id) { return sha256_hex(id).substr(0, 12); }

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 12 || name.front() == ' ' || name.back() == ' ') return false;
    for (unsigned char c : name) if (c < 0x20 || c > 0x7e) return false;
    return true;
}

std::string form_escape(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out.push_back(char(c));
        else { out.push_back('%'); out.push_back(digits[c >> 4]); out.push_back(digits[c & 15]); }
    }
    return out;
}

bool parse_page(const std::string& body, Page& out) {
    const auto lines = lines_of(body);
    if (lines.empty()) return false;
    const auto head = split(lines[0], '\t');
    if (head.size() != 7 || head[0] != "#v1") return false;
    Page page;
    std::uint64_t generated = 0;
    page.board = head[2];
    page.period = head[3];
    if (!to_int(head[4], page.offset) || !to_int(head[5], page.total) || !to_u64(head[6], generated)) return false;
    page.generated = std::int64_t(generated);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto c = split(lines[i], '\t');
        if (c.size() != 5 && c.size() != 6) return false;
        Row row;
        std::uint64_t ts = 0;
        if (!to_int(c[0], row.rank) || !to_u64(c[1], row.score) || !to_u64(c[3], ts)) return false;
        row.name = c[2];
        row.ts = std::int64_t(ts);
        row.pid = c[4];
        row.verified = c.size() == 6 && c[5] == "1";
        page.rows.push_back(std::move(row));
    }
    out = std::move(page);
    return true;
}

bool parse_rank(const std::string& body, MyRank& out) {
    const auto lines = lines_of(body);
    if (lines.empty()) return false;
    if (lines[0] == "-") { out = MyRank{}; return true; }
    const auto c = split(lines[0], '\t');
    if (c.size() != 5) return false;
    MyRank r;
    std::uint64_t ts = 0;
    if (!to_int(c[0], r.rank) || !to_u64(c[1], r.score) || !to_u64(c[3], ts) || !to_int(c[4], r.total)) return false;
    r.found = true;
    r.name = c[2];
    r.ts = std::int64_t(ts);
    out = std::move(r);
    return true;
}

std::vector<Submitted> parse_submit(const std::string& body) {
    std::vector<Submitted> out;
    const auto lines = lines_of(body);
    if (lines.empty() || lines[0] != "ok") return out;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto c = split(lines[i], '\t');
        if (c.size() != 5) continue;
        Submitted s;
        s.board = c[0];
        s.name = c[1];
        if (!to_u64(c[2], s.score) || !to_int(c[4], s.rank)) continue;
        s.fresh = c[3] == "new";
        out.push_back(std::move(s));
    }
    return out;
}

std::string Client::key_of(int kind, const std::string& board, const std::string& period, int offset) {
    return (kind ? "rank_" : "top_") + board + "_" + period + "_" + std::to_string(offset);
}

std::wstring Client::cache_file(const std::string& key) const {
    return config_.data_dir + L"\\cache\\" + widen(key) + L".txt";
}

bool Client::start(const Config& config) {
    stop();
    config_ = config;
    if (config_.page_size < 1 || config_.page_size > 100) config_.page_size = 50;
    CreateDirectoryW(config_.data_dir.c_str(), nullptr);
    CreateDirectoryW((config_.data_dir + L"\\cache").c_str(), nullptr);

    // One install id shared by every recomp on this PC, so a player has a
    // consistent identity. Falls back to the game's own folder.
    std::wstring id_dir = config_.identity_dir;
    if (id_dir.empty()) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
            id_dir = std::wstring(local) + L"\\RecompLeaderboard";
        CoTaskMemFree(local);
    }
    if (id_dir.empty() || (!CreateDirectoryW(id_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS))
        id_dir = config_.data_dir;
    const auto id_path = id_dir + L"\\install-id.txt";
    std::string id;
    read_file(id_path, id);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    bool valid = id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (!valid) {
        unsigned char random[16]{};
        if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
        id = hex(random, sizeof random);
        if (!write_file(id_path, id + "\n")) return false;
    }
    install_id_ = id;
    pid_ = pid_from_install_id(id);
    if (pid_.size() != 12) return false;
    load_disk();
    stop_ = false;
    started_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Client::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

std::string Client::pid() const { return pid_; }

void Client::load_disk() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    std::string text;
    if (read_file(config_.data_dir + L"\\pending.txt", text)) {
        for (const auto& line : lines_of(text)) {
            const auto c = split(line, '\t');
            Pending p;
            if ((c.size() == 3 || c.size() == 5) && valid_name(c[1]) && to_u64(c[2], p.score)) {
                p.board = c[0]; p.name = c[1];
                if (c.size() == 5 && c[3] != "-" && c[3].size() == 64) { p.replay = c[3]; p.sent = c[4] == "1"; }
                pending_.push_back(std::move(p));
            }
        }
    }
    // Cached pages are loaded lazily by key on first access.
}

void Client::save_pending_locked() const {
    std::string text;
    for (const auto& p : pending_)
        text += p.board + "\t" + p.name + "\t" + std::to_string(p.score) + "\t" + (p.replay.empty() ? "-" : p.replay) +
                "\t" + (p.sent ? "1" : "0") + "\n";
    write_file(config_.data_dir + L"\\pending.txt", text);
}

std::wstring Client::replay_file(const std::string& hash) const {
    return config_.data_dir + L"\\replays\\" + widen(hash) + L".bin";
}

void Client::submit(const std::string& board, const std::string& name, std::uint64_t score,
                    const std::vector<std::uint8_t>& replay) {
    if (!started_ || !valid_name(name) || score == 0) return;
    std::string hash;
    if (!replay.empty() && replay.size() <= (1u << 20)) {
        const std::string bytes(replay.begin(), replay.end());
        hash = sha256_hex(bytes);
        CreateDirectoryW((config_.data_dir + L"\\replays").c_str(), nullptr);
        if (hash.size() != 64 || !write_file(replay_file(hash), bytes)) hash.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : pending_) if (p.board == board && p.name == name && p.score == score) return;
        pending_.push_back({board, name, score, hash, false});
        save_pending_locked();
        retry_at_ = 0;
        backoff_ = 15;
    }
    ++revision_;
    wake_.notify_all();
}

void Client::request(const std::string& board, const std::string& period, int offset, bool force) {
    if (!configured()) return;
    const auto key = key_of(0, board, period, offset);
    const auto now = now_unix();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& c = cache_[key];
        if (c.fetched == 0 && c.body.empty()) {
            std::string text;
            if (read_file(cache_file(key), text)) {
                const auto nl = text.find('\n');
                const auto meta = split(text.substr(0, nl), '\t');
                std::uint64_t fetched = 0;
                if (nl != std::string::npos && meta.size() == 2 && to_u64(meta[0], fetched)) {
                    c.body = text.substr(nl + 1); c.etag = meta[1];
                    c.fetched = std::int64_t(fetched);
                }
            }
        }
        if (inflight_[key]) return;
        const int floor = force ? 5 : config_.min_refresh_seconds;
        if (now - c.fetched < floor || now - c.tried < (force ? 5 : 20)) return;
        inflight_[key] = true;
        jobs_.push_back({0, board, period, offset});
    }
    ++revision_;
    wake_.notify_all();
}

void Client::request_rank(const std::string& board, const std::string& period, bool force) {
    if (!configured()) return;
    const auto key = key_of(1, board, period, 0);
    const auto now = now_unix();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& c = cache_[key];
        if (inflight_[key]) return;
        const int floor = force ? 5 : config_.min_refresh_seconds;
        if (now - c.fetched < floor || now - c.tried < (force ? 5 : 20)) return;
        inflight_[key] = true;
        jobs_.push_back({1, board, period, 0});
    }
    wake_.notify_all();
}

bool Client::page(const std::string& board, const std::string& period, int offset, Page& out,
                  std::int64_t* fetched_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(key_of(0, board, period, offset));
    if (it == cache_.end() || it->second.body.empty() || !parse_page(it->second.body, out)) return false;
    if (fetched_unix) *fetched_unix = it->second.fetched;
    return true;
}

bool Client::rank(const std::string& board, const std::string& period, MyRank& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(key_of(1, board, period, 0));
    return it != cache_.end() && !it->second.body.empty() && parse_rank(it->second.body, out);
}

bool Client::loading(const std::string& board, const std::string& period, int offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = inflight_.find(key_of(0, board, period, offset));
    return it != inflight_.end() && it->second;
}

std::size_t Client::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::string Client::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::vector<Submitted> Client::take_submitted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Submitted> out;
    out.swap(submitted_);
    return out;
}

void Client::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        if (!jobs_.empty()) {
            const Job job = jobs_.front();
            jobs_.pop_front();
            lock.unlock();
            fetch(job);
            lock.lock();
            continue;
        }
        if (!pending_.empty() && !config_.url.empty() && now_unix() >= retry_at_) {
            lock.unlock();
            flush();
            lock.lock();
            continue;
        }
        wake_.wait_for(lock, std::chrono::seconds(2));
    }
}

void Client::fetch(const Job& job) {
    const auto key = key_of(job.kind, job.board, job.period, job.offset);
    std::string etag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        etag = cache_[key].etag;
    }
    std::string path = job.kind ? "/v1/rank?" : "/v1/top?";
    path += "g=" + form_escape(config_.game) + "&b=" + form_escape(job.board) + "&p=" + form_escape(job.period);
    if (job.kind) path += "&pid=" + pid_;
    else path += "&o=" + std::to_string(job.offset) + "&n=" + std::to_string(config_.page_size) + "&ext=1";
    std::string body, new_etag;
    const int status = http(L"GET", path, {}, etag, body, new_etag);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& c = cache_[key];
        c.tried = now_unix();
        inflight_[key] = false;
        const bool parsed = status == 200 && (job.kind ? [&] { MyRank r; return parse_rank(body, r); }()
                                                       : [&] { Page p; return parse_page(body, p); }());
        if (parsed) {
            c.body = body; c.etag = new_etag; c.fetched = c.tried;
            if (!job.kind) write_file(cache_file(key), std::to_string(c.fetched) + "\t" + c.etag + "\n" + c.body);
            offline_ = false; error_.clear();
        } else if (status == 304) {
            c.fetched = c.tried;
            offline_ = false; error_.clear();
        } else {
            offline_ = status <= 0;
            error_ = status <= 0 ? "OFFLINE" : "SERVER ERROR " + std::to_string(status);
        }
    }
    ++revision_;
}

void Client::flush() {
    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : pending_)
            if (!p.sent && batch.size() < 25) batch.push_back(p);
    }
    if (batch.empty()) { upload_replay(); return; }
    std::string body = "g=" + form_escape(config_.game) + "&id=" + install_id_ + "&v=" + form_escape(config_.version);
    for (const auto& p : batch)
        body += "&e=" + form_escape(p.board + ":" + p.name + ":" + std::to_string(p.score));
    if (!config_.key.empty()) body += "&h=" + hmac_sha256_hex(config_.key, body);
    std::string reply, etag;
    const int status = http(L"POST", "/v1/submit", body, {}, reply, etag);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool drop = status == 400 || status == 403 || status == 413;
        if (status == 200 || drop) {
            auto results = parse_submit(reply);
            // A new score keeps its replay queued for upload; everything else is finished.
            for (auto& p : pending_) {
                const bool in_batch = std::any_of(batch.begin(), batch.end(), [&](const Pending& b) {
                    return b.board == p.board && b.name == p.name && b.score == p.score; });
                if (!in_batch) continue;
                const bool fresh = !drop && std::any_of(results.begin(), results.end(), [&](const Submitted& r) {
                    return r.fresh && r.board == p.board && r.name == p.name && r.score == p.score; });
                if (fresh && !p.replay.empty()) p.sent = true;
                else p.board.clear();   // marked for removal below
            }
            for (const auto& p : pending_)
                if (p.board.empty() && !p.replay.empty()) {
                    bool shared = false;
                    for (const auto& q : pending_) if (!q.board.empty() && q.replay == p.replay) shared = true;
                    if (!shared) DeleteFileW(replay_file(p.replay).c_str());
                }
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                          [](const Pending& p) { return p.board.empty(); }), pending_.end());
            save_pending_locked();
            for (auto& r : results) {
                // A new score changes the first page and ranks; refetch next time they are viewed.
                for (auto& [k, c] : cache_)
                    if (k.find("_" + r.board + "_") != std::string::npos) c.fetched = 0;
                submitted_.push_back(std::move(r));
            }
            if (drop) error_ = "SCORE REJECTED (" + std::to_string(status) + ")";
            else { offline_ = false; error_.clear(); }
            backoff_ = 15;
            retry_at_ = 0;
        } else {
            offline_ = status <= 0;
            error_ = status <= 0 ? "OFFLINE" : status == 429 ? "UPLOAD LIMIT - RETRYING" : "SERVER ERROR " + std::to_string(status);
            retry_at_ = now_unix() + backoff_;
            backoff_ = std::min(backoff_ * 2, 1800);
        }
    }
    ++revision_;
}

// One queued replay per call, sent after its score was accepted as new. A replay
// the server cannot take (rank too low, already stored, malformed) is dropped;
// network trouble backs off like score uploads.
void Client::upload_replay() {
    Pending item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.sent && !p.replay.empty(); });
        if (it == pending_.end()) {
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.sent; }), pending_.end());
            save_pending_locked();
            return;
        }
        item = *it;
    }
    std::string bytes;
    int status = 404;
    std::string reply, etag;
    if (read_file(replay_file(item.replay), bytes) && !bytes.empty()) {
        const std::string path = "/v1/replay?g=" + form_escape(config_.game) + "&id=" + install_id_ + "&b=" + form_escape(item.board) +
                                 "&n=" + form_escape(item.name) + "&s=" + std::to_string(item.score);
        status = http(L"POST", path, bytes, {}, reply, etag, L"application/octet-stream");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status == 200 || (status >= 400 && status < 500 && status != 429)) {
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) {
                return p.sent && p.board == item.board && p.name == item.name && p.score == item.score; }), pending_.end());
            bool shared = false;
            for (const auto& q : pending_) if (q.replay == item.replay) shared = true;
            if (!shared) DeleteFileW(replay_file(item.replay).c_str());
            save_pending_locked();
            backoff_ = 15;
            retry_at_ = 0;
            offline_ = false;
        } else {
            offline_ = status <= 0;
            retry_at_ = now_unix() + backoff_;
            backoff_ = std::min(backoff_ * 2, 1800);
        }
    }
    ++revision_;
}

int Client::http(const wchar_t* method, const std::string& path, const std::string& body,
                 const std::string& etag, std::string& out_body, std::string& out_etag,
                 const wchar_t* content_type) {
    out_body.clear();
    out_etag.clear();
    const auto endpoint = parse_url(config_.url);
    if (!endpoint.ok) return -1;
    const std::wstring agent = L"recomp-leaderboard/1 (" + widen(config_.game) + L")";
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 10000);
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    int status = -1;
    HINTERNET connect = WinHttpConnect(session, endpoint.host.c_str(), endpoint.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, method, (endpoint.base + widen(path)).c_str(), nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     endpoint.secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        std::wstring headers;
        if (!body.empty()) headers += std::wstring(L"Content-Type: ") + content_type + L"\r\n";
        if (!etag.empty()) headers += L"If-None-Match: " + widen(etag) + L"\r\n";
        const BOOL sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                             headers.empty() ? 0 : DWORD(-1),
                                             body.empty() ? WINHTTP_NO_REQUEST_DATA : LPVOID(body.data()),
                                             DWORD(body.size()), DWORD(body.size()), 0);
        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            wchar_t tag[128]{};
            DWORD tag_size = sizeof tag;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, tag, &tag_size,
                                    WINHTTP_NO_HEADER_INDEX)) out_etag = narrow(tag);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                if (out_body.size() + available > (1u << 20)) { code = 0; break; }
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                out_body.append(chunk.data(), read);
            }
            status = code ? int(code) : -1;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

}  // namespace rl::lb
