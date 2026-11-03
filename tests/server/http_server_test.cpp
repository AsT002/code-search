// The HTTP server: same results as the CLI, cancelling, timeouts, updates
// during a search, and turning away requests from other sites.

#include "http_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/temp_repo.hpp"
#include "codesearch/app/repo_manager.hpp"
#include "codesearch/util/process.hpp"

namespace codesearch {
namespace {

using Json = nlohmann::json;
using testing::TempDir;
using testing::TempRepo;

Cloner noGitHub() {
    return [](const std::string&, const std::filesystem::path&) {
        throw std::logic_error("this test shouldn't clone");
    };
}

// A server on a free port of 127.0.0.1, answering on its own thread.
class RunningServer {
public:
    explicit RunningServer(ServerConfig config) : server_(std::move(config)) {
        port_ = server_.bind(0);
        thread_ = std::thread([this] {
            server_.run();
        });
    }

    ~RunningServer() {
        server_.stop();
        thread_.join();
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;
    RunningServer(RunningServer&&) = delete;
    RunningServer& operator=(RunningServer&&) = delete;

    int port() const { return port_; }

    httplib::Client client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_read_timeout(std::chrono::seconds(20));
        return client;
    }

private:
    HttpServer server_;
    int port_ = 0;
    std::thread thread_;
};

// A workspace with two added repos, "alpha" and "beta".
class ServerTest : public ::testing::Test {
protected:
    ServerTest() {
        alpha_.write("a.txt", "first line\nsecond Target line\n");
        alpha_.write("crlf.txt", "target with CRLF\r\nnext\r\n");
        alpha_.write("bad-utf8.txt", "target \xFF\xFE bytes\n");
        alpha_.write("src/main.go", "package main // target\nfunc main() {}\n");
        alpha_.commit();
        beta_.write("b.txt", "another target here\nand <script>alert(1)</script>\n");
        beta_.write("deep/x.py", "def target():\n    return 'fo' + 'foo'\n");
        beta_.commit();
        RepoManager manager(workspace(), noGitHub());
        manager.add(alpha_.dir().string(), "alpha");
        manager.add(beta_.dir().string(), "beta");
    }

    Workspace workspace() const { return Workspace(home_.dir() / "ws"); }

    ServerConfig config() const {
        return ServerConfig{.workspace = workspace(), .cloner = noGitHub()};
    }

    // What `code-search search --json <args>` prints for this workspace.
    ProcessResult cliSearch(std::vector<std::string> args) const {
        args.insert(args.begin(), {"search", "--json"});
        return runProcess(CODE_SEARCH_BIN, args,
                          {"CODE_SEARCH_HOME=" + workspace().root().string()});
    }

    TempDir home_;
    TempRepo alpha_;
    TempRepo beta_;
};

std::string headerOf(const httplib::Result& result, const std::string& name) {
    return result->get_header_value(name);
}

// One query, sent to both the CLI and the server.
struct SameQuery {
    std::vector<std::string> cliArgs;
    httplib::Params params;
    int httpStatus = 200;
};

TEST_F(ServerTest, GivesTheSameResultsAsTheCli) {
    const RunningServer server(config());
    httplib::Client client = server.client();
    const std::vector<SameQuery> queries = {
        {{"target"}, {{"q", "target"}}},
        {{"-C", "1", "target"}, {{"q", "target"}, {"context", "1"}}},
        {{"--max-results", "2", "target"}, {{"q", "target"}, {"max_results", "2"}}},
        {{"-l", "target"}, {{"q", "target"}, {"files_only", "true"}}},
        {{"--case-sensitive", "Target"}, {{"q", "Target"}, {"case_sensitive", "true"}}},
        {{"--repo", "beta", "target"}, {{"q", "target"}, {"repo", "beta"}}},
        {{"/fo+/ or lang:go"}, {{"q", "/fo+/ or lang:go"}}},
        {{"(repo:alpha second) or (repo:beta script)"},
         {{"q", "(repo:alpha second) or (repo:beta script)"}}},
        {{"--", "-target"}, {{"q", "-target"}}},
        {{"nothing matches this"}, {{"q", "nothing matches this"}}},
        // Errors before the search starts: the same document, with a status.
        {{"(target"}, {{"q", "(target"}}, 400},
        {{"/a(/"}, {{"q", "/a(/"}}, 400},
        {{"repo:gamma x"}, {{"q", "repo:gamma x"}}, 404},
    };
    for (const SameQuery& query : queries) {
        SCOPED_TRACE(query.params.find("q")->second);
        const ProcessResult cli = cliSearch(query.cliArgs);
        const httplib::Result http = client.Get("/api/v1/search", query.params, httplib::Headers{});
        ASSERT_TRUE(http);
        EXPECT_EQ(http->status, query.httpStatus);
        EXPECT_EQ(http->body, cli.out);
        EXPECT_EQ(headerOf(http, "Content-Type"), "application/json");
    }
}

TEST_F(ServerTest, RejectsBadSearchParameters) {
    const RunningServer server(config());
    httplib::Client client = server.client();
    const std::vector<std::string> bad = {
        "/api/v1/search",                     // no query
        "/api/v1/search?q=x&q=y",             // twice
        "/api/v1/search?q=x&max_results=-1",  // not a count
        "/api/v1/search?q=x&context=1e3",     // not a count
        "/api/v1/search?q=x&files_only=yes",  // not true or false
        "/api/v1/search?q=x&repo=",           // empty repo
        "/api/v1/search?q=x&color=always",    // unknown
    };
    for (const std::string& path : bad) {
        SCOPED_TRACE(path);
        const httplib::Result result = client.Get(path);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 400);
        const Json body = Json::parse(result->body);
        EXPECT_EQ(body["status"], "error");
        EXPECT_EQ(body["files"], Json::array());
    }
}

TEST_F(ServerTest, SaysWhenNoRepoIsAdded) {
    const TempDir empty;
    const RunningServer server(
        ServerConfig{.workspace = Workspace(empty.dir() / "ws"), .cloner = noGitHub()});
    const httplib::Result result = server.client().Get("/api/v1/search?q=x");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
    EXPECT_NE(result->body.find("no repos added yet"), std::string::npos);
}

TEST_F(ServerTest, ReportsItsVersion) {
    const RunningServer server(config());
    const httplib::Result result = server.client().Get("/api/v1/version");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body)["api"], 1);
}

TEST_F(ServerTest, AddsUpdatesAndRemovesRepos) {
    const RunningServer server(config());
    httplib::Client client = server.client();

    TempRepo gamma;
    gamma.write("g.txt", "gamma needle\n");
    gamma.commit();
    const std::string addBody = Json{{"source", gamma.dir().string()}, {"name", "gamma"}}.dump();
    httplib::Result result = client.Post("/api/v1/repos", addBody, "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 201) << result->body;
    EXPECT_EQ(Json::parse(result->body)["report"]["indexed"]["files"], 1);

    // Adding it again under another name fails, with a reason.
    result = client.Post("/api/v1/repos",
                         Json{{"source", gamma.dir().string()}, {"name", "gamma2"}}.dump(),
                         "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 422);
    EXPECT_EQ(Json::parse(result->body)["error"]["code"], "failed");

    result = client.Get("/api/v1/repos");
    ASSERT_TRUE(result);
    const Json repos = Json::parse(result->body)["repos"];
    ASSERT_EQ(repos.size(), 3U);
    EXPECT_EQ(repos[2]["name"], "gamma");
    EXPECT_EQ(repos[2]["source"], "local");

    result = client.Post("/api/v1/repos/gamma/update", "{}", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body)["rebuilt"], false);

    gamma.write("g.txt", "gamma changed\n");
    gamma.commit();
    result = client.Post("/api/v1/repos/gamma/update", "", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body)["rebuilt"], true);
    result = client.Get("/api/v1/search?q=changed&files_only=true");
    ASSERT_TRUE(result);
    EXPECT_NE(result->body.find("\"g.txt\""), std::string::npos);

    result = client.Delete("/api/v1/repos/gamma");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    result = client.Delete("/api/v1/repos/gamma");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
    EXPECT_EQ(Json::parse(result->body)["error"]["code"], "unknown_repo");
    result = client.Post("/api/v1/repos/gamma/update", "{}", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
}

TEST_F(ServerTest, RejectsBadChangeRequests) {
    const RunningServer server(config());
    httplib::Client client = server.client();
    struct Bad {
        std::string body;
        std::string field;  // what the message should name
    };
    const std::vector<Bad> bad = {
        {"not json", "JSON object"},   {"[1,2]", "JSON object"},
        {R"({"source":1})", "source"}, {R"({"source":"x","extra":true})", "extra"},
        {R"({"name":"x"})", "source"},
    };
    for (const Bad& request : bad) {
        SCOPED_TRACE(request.body);
        const httplib::Result result =
            client.Post("/api/v1/repos", request.body, "application/json");
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 400);
        EXPECT_NE(result->body.find(request.field), std::string::npos) << result->body;
    }
    const httplib::Result result =
        client.Post("/api/v1/repos/alpha/update", R"({"rebuild":"yes"})", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 400);
}

TEST_F(ServerTest, TurnsAwayRequestsFromOtherSites) {
    const RunningServer server(config());
    httplib::Client client = server.client();
    const std::string port = std::to_string(server.port());

    // DNS rebinding: a site's own name pointed at 127.0.0.1.
    httplib::Result result =
        client.Get("/api/v1/repos", httplib::Headers{{"Host", "evil.example:" + port}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 403);

    // A page on another site, even just reading.
    result = client.Get("/api/v1/search?q=x", httplib::Headers{{"Origin", "http://evil.example"}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 403);
    result =
        client.Post("/api/v1/repos", {{"Origin", "http://evil.example"}}, "{}", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 403);

    // An HTML form can only send these types, so changes need JSON.
    for (const std::string type :
         {"text/plain", "application/x-www-form-urlencoded", "multipart/form-data"}) {
        result = client.Post("/api/v1/repos/alpha/update", "{}", type);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 415) << type;
    }
    // Nothing above changed anything.
    EXPECT_EQ(loadRegistry(workspace()).repos.size(), 2U);

    // The server's own pages are fine, by either name.
    result = client.Post("/api/v1/repos/alpha/update",
                         {{"Origin", "http://localhost:" + port}, {"Host", "localhost:" + port}},
                         "{}", "application/json; charset=utf-8");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
}

TEST_F(ServerTest, LimitsTheRequestBody) {
    ServerConfig settings = config();
    settings.maxBodyBytes = 1024;
    const RunningServer server(std::move(settings));
    const std::string big = R"({"source":")" + std::string(2000, 'x') + R"("})";
    const httplib::Result result = server.client().Post("/api/v1/repos", big, "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 413);
}

TEST_F(ServerTest, SendsSafetyHeaders) {
    const RunningServer server(config());
    const httplib::Result result = server.client().Get("/api/v1/search?q=script");
    ASSERT_TRUE(result);
    EXPECT_EQ(headerOf(result, "X-Content-Type-Options"), "nosniff");
    EXPECT_NE(headerOf(result, "Content-Security-Policy").find("default-src 'self'"),
              std::string::npos);
    // File content is sent as JSON text; it's never turned into HTML here.
    EXPECT_NE(result->body.find("<script>alert(1)</script>"), std::string::npos);
    EXPECT_EQ(headerOf(result, "Content-Type"), "application/json");
}

TEST_F(ServerTest, ServesTheUiFolder) {
    const TempDir ui;
    testing::writeFile(ui.file("index.html"), "<!doctype html><title>ui</title>");
    ServerConfig settings = config();
    settings.uiDir = ui.dir();
    const RunningServer server(std::move(settings));
    const httplib::Result result = server.client().Get("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->body, "<!doctype html><title>ui</title>");
    // The API still works next to it.
    EXPECT_EQ(server.client().Get("/api/v1/version")->status, 200);
}

TEST_F(ServerTest, ExplainsAMissingUi) {
    const RunningServer server(config());
    const httplib::Result result = server.client().Get("/");
    ASSERT_TRUE(result);
    EXPECT_NE(result->body.find("npm --prefix web run build"), std::string::npos);
}

// Blocks a search's thread until the test lets it go.
class Gate {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        reached_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] {
            return open_;
        });
    }
    void waitUntilReached() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] {
            return reached_;
        });
    }
    void open() {
        const std::scoped_lock lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool reached_ = false;
    bool open_ = false;
};

// Remembers how searches ended.
class Statuses {
public:
    void add(std::string_view status) {
        const std::scoped_lock lock(mutex_);
        statuses_.emplace_back(status);
        changed_.notify_all();
    }
    std::string waitForOne() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(20), [this] {
            return !statuses_.empty();
        });
        return statuses_.empty() ? "none" : statuses_.front();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> statuses_;
};

TEST_F(ServerTest, StopsASearchWhenTheClientGoesAway) {
    TempRepo many;
    for (int i = 0; i < 200; ++i) {
        many.write("f" + std::to_string(1000 + i) + ".txt", "needle\n");
    }
    many.commit();
    RepoManager(workspace(), noGitHub()).add(many.dir().string(), "many");

    Gate gate;
    Statuses statuses;
    std::atomic<int> filesSent = 0;
    ServerConfig settings = config();
    settings.beforeResultFile = [&] {
        if (++filesSent == 1) {
            gate.wait();
        }
    };
    settings.onSearchDone = [&](std::string_view status) {
        statuses.add(status);
    };
    const RunningServer server(std::move(settings));

    // The client gives up waiting and closes the connection.
    std::thread request([&] {
        httplib::Client client = server.client();
        client.set_read_timeout(std::chrono::milliseconds(200));
        EXPECT_FALSE(client.Get("/api/v1/search?q=needle"));
    });
    gate.waitUntilReached();
    request.join();
    // Give the watchdog, which looks every 50 ms, time to see it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    gate.open();

    const std::string status = statuses.waitForOne();
    EXPECT_TRUE(status == "cancelled" || status == "disconnected") << status;
    EXPECT_LT(filesSent, 200);
}

TEST_F(ServerTest, StopsASearchThatTakesTooLong) {
    Statuses statuses;
    std::atomic<int> filesSent = 0;
    ServerConfig settings = config();
    settings.searchTimeout = std::chrono::milliseconds(100);
    settings.beforeResultFile = [&] {
        if (++filesSent == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    };
    settings.onSearchDone = [&](std::string_view status) {
        statuses.add(status);
    };
    const RunningServer server(std::move(settings));

    const httplib::Result result = server.client().Get("/api/v1/search?q=target");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);  // the results had started
    const Json body = Json::parse(result->body);
    EXPECT_EQ(body["status"], "error");
    EXPECT_NE(body["error"].get<std::string>().find("took longer than 100 ms"), std::string::npos);
    EXPECT_GE(body["files"].size(), 1U);  // what was found before the time ran out
    EXPECT_LT(body["files"].size(), 5U);
    EXPECT_EQ(statuses.waitForOne(), "error");
}

TEST_F(ServerTest, ASearchKeepsItsSnapshotWhileAnUpdateRuns) {
    // What the search should give: alpha as it is now.
    const std::string before = cliSearch({"--repo", "alpha", "target"}).out;

    Gate gate;
    ServerConfig settings = config();
    std::atomic<int> filesSent = 0;
    settings.beforeResultFile = [&] {
        if (++filesSent == 1) {
            gate.wait();
        }
    };
    const RunningServer server(std::move(settings));

    std::string during;
    std::thread request([&] {
        const httplib::Result result = server.client().Get("/api/v1/search?q=target&repo=alpha");
        ASSERT_TRUE(result);
        during = result->body;
    });
    gate.waitUntilReached();

    // Change alpha and update it while the search is partway. The update
    // deletes the index file the search has open.
    alpha_.write("a.txt", "no longer matching\n");
    alpha_.write("new.txt", "a new target\n");
    alpha_.commit();
    const UpdateResult updated = RepoManager(workspace(), noGitHub()).update("alpha", false);
    EXPECT_FALSE(updated.reason.empty());
    gate.open();
    request.join();

    EXPECT_EQ(during, before);
    // A new search sees the update, not the file the cache had open.
    const httplib::Result after = server.client().Get("/api/v1/search?q=target&repo=alpha");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->body, cliSearch({"--repo", "alpha", "target"}).out);
    EXPECT_NE(after->body.find("new.txt"), std::string::npos);
    EXPECT_EQ(after->body.find("\"a.txt\""), std::string::npos);
}

TEST_F(ServerTest, AnswersSearchesAtTheSameTime) {
    const RunningServer server(config());
    const std::string expected = cliSearch({"target"}).out;
    std::vector<std::thread> threads;
    std::atomic<int> same = 0;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            for (int k = 0; k < 10; ++k) {
                const httplib::Result result = server.client().Get("/api/v1/search?q=target");
                if (result && result->body == expected) {
                    ++same;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(same, 80);
}

}  // namespace
}  // namespace codesearch
