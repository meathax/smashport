// Recomp global leaderboard client (Windows: WinHTTP + BCrypt, no other deps).
// Shared by every recomp; canonical copy: D:/Arcade/AI/recomp-leaderboard/client.
//
// Bandwidth policy: nothing is fetched until the player opens a leaderboard
// page; a page or rank is refetched at most once per min_refresh_seconds
// (unless forced), revalidated with ETag (304 = no body), and cached on disk
// so offline players still see the last known rankings. Scores are queued on
// disk and sent in batches with exponential backoff.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rl::lb {

struct Row {
    int rank = 0;
    std::uint64_t score = 0;
    std::string name;
    std::int64_t ts = 0;
    std::string pid;
};

struct Page {
    std::string board, period;
    int offset = 0, total = 0;
    std::int64_t generated = 0;
    std::vector<Row> rows;
};

struct MyRank {
    bool found = false;
    int rank = 0, total = 0;
    std::uint64_t score = 0;
    std::string name;
    std::int64_t ts = 0;
};

struct Submitted {
    std::string board, name;
    std::uint64_t score = 0;
    bool fresh = false;
    int rank = 0;
};

struct Config {
    std::wstring url;            // e.g. https://recomp-leaderboard.NAME.workers.dev
    std::string game;            // e.g. smashtv
    std::string version;         // build label sent with submissions
    std::string key;             // optional HMAC key (matches Worker secret KEY_<game>)
    std::wstring data_dir;       // per-game folder for queue and cache
    std::wstring identity_dir;   // optional; default %LOCALAPPDATA%\RecompLeaderboard (shared install id)
    int min_refresh_seconds = 120;   // reopening a board within this uses the cache
    int page_size = 50;
};

// Pure helpers (unit-testable without network).
bool parse_page(const std::string& body, Page& out);
bool parse_rank(const std::string& body, MyRank& out);
std::vector<Submitted> parse_submit(const std::string& body);
std::string form_escape(const std::string& s);
std::string sha256_hex(const std::string& s);
std::string hmac_sha256_hex(const std::string& key, const std::string& msg);
std::string pid_from_install_id(const std::string& install_id_hex);
bool valid_name(const std::string& name);

class Client {
public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() { stop(); }

    bool start(const Config& config);
    void stop();
    bool started() const { return started_; }
    bool configured() const { return started_ && !config_.url.empty(); }
    std::string pid() const;

    // Queue a score (persisted). Duplicate (board,name,score) in the queue is ignored.
    void submit(const std::string& board, const std::string& name, std::uint64_t score);
    // Fetch a page / the local player's rank unless fresh (force skips the age check).
    void request(const std::string& board, const std::string& period, int offset, bool force = false);
    void request_rank(const std::string& board, const std::string& period, bool force = false);

    bool page(const std::string& board, const std::string& period, int offset, Page& out,
              std::int64_t* fetched_unix = nullptr) const;
    bool rank(const std::string& board, const std::string& period, MyRank& out) const;
    bool loading(const std::string& board, const std::string& period, int offset) const;
    std::size_t pending() const;
    bool offline() const { return offline_; }
    std::string last_error() const;
    std::vector<Submitted> take_submitted();
    std::uint64_t revision() const { return revision_; }
    int page_size() const { return config_.page_size; }

private:
    struct Job { int kind = 0; std::string board, period; int offset = 0; };
    struct Cached { std::string body, etag; std::int64_t fetched = 0; std::int64_t tried = 0; };
    struct Pending { std::string board, name; std::uint64_t score = 0; };

    void run();
    void fetch(const Job& job);
    void flush();
    void save_pending_locked() const;
    void load_disk();
    std::wstring cache_file(const std::string& key) const;
    static std::string key_of(int kind, const std::string& board, const std::string& period, int offset);
    int http(const wchar_t* method, const std::string& path, const std::string& body,
             const std::string& etag, std::string& out_body, std::string& out_etag);

    Config config_;
    std::string install_id_, pid_;
    bool started_ = false;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::map<std::string, Cached> cache_;
    std::map<std::string, bool> inflight_;
    std::vector<Pending> pending_;
    std::vector<Submitted> submitted_;
    std::string error_;
    std::int64_t retry_at_ = 0;
    int backoff_ = 15;
    std::atomic<bool> stop_{false}, offline_{false};
    std::atomic<std::uint64_t> revision_{1};
    std::thread thread_;
};

}  // namespace rl::lb
