#include "http_server.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "codesearch/app/index_cache.hpp"
#include "codesearch/app/search_service.hpp"
#include "codesearch/index/file_filter.hpp"
#include "codesearch/index/index_file_error.hpp"
#include "codesearch/query/parser.hpp"
#include "codesearch/search/json_output.hpp"
#include "codesearch/util/thread_pool.hpp"
#include "codesearch/version.hpp"

namespace codesearch {

namespace {

using Json = nlohmann::json;

constexpr int kApiVersion = 1;

// Search results are sent in pieces of about this size.
constexpr std::size_t kChunkBytes = std::size_t{16} << 10U;  // 16 KiB

// ---------------------------------------------------------------------------
// Answers

// JSON text. Bytes that aren't valid UTF-8 (in a repo's folder name, say)
// become U+FFFD instead of making dump() throw.
std::string jsonText(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void sendJson(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(jsonText(body) + "\n", "application/json");
}

// Every route except search answers errors like this:
//
//   {"error":{"code":"unknown_repo","message":"no repo named x"}}
void sendError(httplib::Response& res, int status, const std::string& code,
               const std::string& message) {
    sendJson(res, status, {{"error", {{"code", code}, {"message", message}}}});
}

// Search answers errors with the same document `search --json` prints:
//
//   {"files":[],"status":"error","error":"..."}
void sendSearchError(httplib::Response& res, int status, const std::string& message) {
    std::ostringstream out;
    JsonResultWriter(out).fail(message);
    res.status = status;
    res.set_content(out.str(), "application/json");
}

Json repoJson(const RepoEntry& repo) {
    return {
        {"name", repo.name},
        {"source", repo.source == RepoSource::Local ? "local" : "github"},
        {"location", repo.location},
        {"commit", repo.index.commit.hex()},
    };
}

Json countJson(const FileCount& count) {
    return {{"files", count.files}, {"bytes", count.bytes}};
}

Json reportJson(const IngestReport& report) {
    Json skipped = Json::object();
    for (const auto& [reason, count] : report.skipped) {
        skipped[std::string(skipReasonName(reason))] = countJson(count);
    }
    return {{"indexed", countJson(report.indexed)}, {"skipped", skipped}};
}

// ---------------------------------------------------------------------------
// Reading requests

// Reads a whole non-negative number. Nothing if `text` is anything else.
std::optional<std::size_t> parseCount(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parseBool(std::string_view text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

// Sets one search setting from a URL parameter. Returns what's wrong, or
// "" if nothing is.
std::string readSearchParameter(const std::string& key, const std::string& value,
                                SearchSettings& settings) {
    if (key == "q") {
        settings.query = value;
    } else if (key == "repo") {
        if (value.empty()) {
            return "repo can't be empty";  // it would mean "every repo"
        }
        settings.repo = value;
    } else if (key == "case_sensitive" || key == "files_only") {
        const std::optional<bool> flag = parseBool(value);
        if (!flag.has_value()) {
            return key + " must be true or false";
        }
        (key == "case_sensitive" ? settings.caseSensitive : settings.filesOnly) = flag.value();
    } else if (key == "max_results" || key == "context") {
        const std::optional<std::size_t> count = parseCount(value);
        if (!count.has_value()) {
            return key + " must be a whole number, 0 or more";
        }
        (key == "max_results" ? settings.maxResults : settings.contextLines) = count.value();
    } else {
        return "unknown parameter " + key;
    }
    return "";
}

// Fills `settings` from the URL's parameters, like
// ?q=foo&repo=demo&max_results=10. Returns what's wrong, or "" if nothing is.
std::string readSearchSettings(const httplib::Request& req, SearchSettings& settings) {
    for (const auto& [key, value] : req.params) {
        if (req.params.count(key) > 1) {
            return "the parameter " + key + " is given more than once";
        }
        std::string problem = readSearchParameter(key, value, settings);
        if (!problem.empty()) {
            return problem;
        }
    }
    if (!req.has_param("q")) {
        return "the query is missing. Pass it as ?q=...";
    }
    return "";
}

// The request's body as a JSON object with no keys other than `allowed`.
// An empty body is an empty object. If the body is anything else, sends a
// 400 answer and returns nothing.
std::optional<Json> readBody(const httplib::Request& req, httplib::Response& res,
                             const std::vector<std::string>& allowed) {
    if (req.body.empty()) {
        return Json::object();
    }
    // Without exceptions: a body that isn't JSON gives a "discarded" value.
    Json body = Json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        sendError(res, 400, "bad_request", "the body must be a JSON object");
        return std::nullopt;
    }
    for (const auto& [key, value] : body.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            sendError(res, 400, "bad_request", "unknown field " + key);
            return std::nullopt;
        }
    }
    return body;
}

// The text of field `key`, "" if it's missing. Sends a 400 answer and
// returns nothing if it isn't text.
std::optional<std::string> stringField(const Json& body, const std::string& key,
                                       httplib::Response& res) {
    if (!body.contains(key)) {
        return std::string();
    }
    if (!body[key].is_string()) {
        sendError(res, 400, "bad_request", key + " must be a string");
        return std::nullopt;
    }
    return body[key].get<std::string>();
}

// The media type of a Content-Type header, without parameters like
// "; charset=utf-8", in lower case.
std::string mediaType(const std::string& contentType) {
    std::string type;
    for (const char c : contentType) {
        if (c == ';') {
            break;
        }
        if (c != ' ') {
            type += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    return type;
}

const char* const kNoUiPage = R"(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>code-search</title></head>
<body><p>The web UI isn't built yet. In the code-search folder, run:</p>
<pre>npm --prefix web ci
npm --prefix web run build</pre>
<p>then start <code>code-search serve</code> again. The API works without it, under /api/v1/.</p>
</body></html>
)";

// Runs a change to the repos, and answers its errors: 404 for a repo that
// isn't added, 422 for anything else that stopped it, like a folder that
// isn't a Git repo or a name that's taken. The message says which.
void change(httplib::Response& res, const std::function<void()>& work) {
    try {
        work();
    } catch (const UnknownRepoError& error) {
        sendError(res, 404, "unknown_repo", error.what());
    } catch (const std::exception& error) {
        sendError(res, 422, "failed", error.what());
    }
}

// ---------------------------------------------------------------------------
// Stopping searches

// A search being sent. The watchdog cancels it when it runs too long or
// the client goes away.
struct RunningSearch {
    std::atomic<bool> cancelled = false;
    std::atomic<bool> timedOut = false;
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> connectionClosed;  // from httplib; true once the client is gone
};

// A thread that looks at each running search every 50 ms. The search
// checks its `cancelled` flag before each file, but nothing else would set
// it while no result is being sent, like during a long search that finds
// little.
class Watchdog {
public:
    Watchdog()
        : thread_([this] {
              loop();
          }) {}

    ~Watchdog() {
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    void add(RunningSearch* search) {
        const std::scoped_lock lock(mutex_);
        searches_.push_back(search);
    }

    // After this returns, the watchdog never touches `search` again. The
    // search must be removed before its connection is closed, since
    // connectionClosed looks at the connection's socket.
    void remove(RunningSearch* search) {
        const std::scoped_lock lock(mutex_);
        std::erase(searches_, search);
    }

    void cancelAll() {
        const std::scoped_lock lock(mutex_);
        for (RunningSearch* search : searches_) {
            search->cancelled = true;
        }
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            for (RunningSearch* search : searches_) {
                if (std::chrono::steady_clock::now() >= search->deadline) {
                    search->timedOut = true;
                    search->cancelled = true;
                } else if (search->connectionClosed()) {
                    search->cancelled = true;
                }
            }
            // Sleeps 50 ms, or less if the destructor wakes it.
            wake_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return stopping_;
            });
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::vector<RunningSearch*> searches_;
    std::thread thread_;  // last, so it starts after the members it uses
};

}  // namespace

// ---------------------------------------------------------------------------
// The server

class HttpServer::Impl {
public:
    explicit Impl(ServerConfig config)
        : config_(std::move(config)),
          pool_(startPool()),
          cache_(config_.maxOpenIndexes),
          service_(config_.workspace, pool_.get(), &cache_) {
        setUp();
    }

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int bind(int port) {
        if (port == 0) {
            port_ = server_.bind_to_any_port("127.0.0.1");
        } else {
            port_ = server_.bind_to_port("127.0.0.1", port) ? port : -1;
        }
        if (port_ < 0) {
            throw std::runtime_error("could not listen on 127.0.0.1:" + std::to_string(port) +
                                     ". Is another program using that port?");
        }
        return port_;
    }

    void run() {
        // listen_after_bind() also returns false when stop() came first.
        if (!server_.listen_after_bind() && !stopping_) {
            throw std::runtime_error("the server stopped with an error");
        }
    }

    void stop() {
        stopping_ = true;
        watchdog_.cancelAll();
        server_.stop();
    }

private:
    void setUp();
    httplib::Server::HandlerResponse checkRequest(const httplib::Request& req,
                                                  httplib::Response& res) const;
    void search(const httplib::Request& req, httplib::Response& res);
    void streamSearch(PreparedSearch& search, const std::function<bool()>& connectionClosed,
                      httplib::DataSink& sink);
    void listRepos(httplib::Response& res) const;
    void addRepo(const httplib::Request& req, httplib::Response& res) const;
    void updateRepo(const httplib::Request& req, httplib::Response& res) const;
    void removeRepo(const httplib::Request& req, httplib::Response& res) const;

    ServerConfig config_;
    std::unique_ptr<ThreadPool> pool_;  // checks files for every search; may be nullptr
    IndexCache cache_;
    SearchService service_;
    Watchdog watchdog_;
    int port_ = -1;
    std::atomic<bool> stopping_ = false;
    // Last, so it's destroyed first: its threads may still use the members above.
    httplib::Server server_;
};

void HttpServer::Impl::setUp() {
    // Answer at most requestThreads requests at once, and turn away more
    // than maxQueuedRequests waiting, instead of starting a thread for each.
    const std::size_t threads = config_.requestThreads;
    const std::size_t queued = config_.maxQueuedRequests;
    // httplib takes ownership of the pool, so this must be a plain `new`.
    server_.new_task_queue = [threads, queued] {
        return new httplib::ThreadPool(threads, threads,
                                       queued);  // NOLINT(cppcoreguidelines-owning-memory)
    };
    server_.set_payload_max_length(config_.maxBodyBytes);
    server_.set_read_timeout(10);
    // A client that stops reading for this long is taken as gone, and its
    // search stops.
    server_.set_write_timeout(10);

    server_.set_default_headers({
        // Browsers must take each answer as the type it says, so JSON can
        // never run as a script.
        {"X-Content-Type-Options", "nosniff"},
        // The UI loads only its own files, and no other site can frame it.
        {
            "Content-Security-Policy",
            "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
        },
        {"Referrer-Policy", "no-referrer"},
        {"Cache-Control", "no-store"},
    });

    server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        return checkRequest(req, res);
    });
    server_.set_exception_handler([](const httplib::Request& /*req*/, httplib::Response& res,
                                     const std::exception_ptr& error) {
        // Rethrowing is the only way to see what an exception_ptr holds.
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            sendError(res, 500, "internal", e.what());
        } catch (...) {
            sendError(res, 500, "internal", "unknown error");
        }
    });
    server_.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
        // Errors httplib finds itself, like a body over the limit (413), or
        // a route that doesn't exist (404). Leaves our own error bodies alone.
        if (res.body.empty()) {
            const std::string code = res.status == 404 ? "not_found" : "bad_request";
            sendError(res, res.status, code, httplib::status_message(res.status));
        }
    });

    server_.Get("/api/v1/version", [](const httplib::Request& /*req*/, httplib::Response& res) {
        sendJson(res, 200, {{"version", std::string(version())}, {"api", kApiVersion}});
    });
    server_.Get("/api/v1/search", [this](const httplib::Request& req, httplib::Response& res) {
        search(req, res);
    });
    server_.Get("/api/v1/repos", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        listRepos(res);
    });
    server_.Post("/api/v1/repos", [this](const httplib::Request& req, httplib::Response& res) {
        addRepo(req, res);
    });
    server_.Post("/api/v1/repos/:name/update",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     updateRepo(req, res);
                 });
    server_.Delete("/api/v1/repos/:name",
                   [this](const httplib::Request& req, httplib::Response& res) {
                       removeRepo(req, res);
                   });

    std::error_code error;
    if (!config_.uiDir.empty() &&
        std::filesystem::is_regular_file(config_.uiDir / "index.html", error)) {
        server_.set_mount_point("/", config_.uiDir.string());
    } else {
        server_.Get("/", [](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_content(kNoUiPage, "text/html; charset=utf-8");
        });
    }
}

// Runs before every route. Turns away requests that a web page on another
// site could have sent:
//
// - The Host must be this server's own address. Otherwise a site could
//   point its own name at 127.0.0.1 (DNS rebinding) and read the answers.
// - An Origin, which browsers send with cross-site requests, must be this
//   server's own.
// - A POST must send JSON. A plain HTML form on another site can't, and a
//   script there would first have to ask, which this server never allows.
httplib::Server::HandlerResponse HttpServer::Impl::checkRequest(const httplib::Request& req,
                                                                httplib::Response& res) const {
    const std::string port = std::to_string(port_);
    const std::string host = req.get_header_value("Host");
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
        sendError(res, 403, "forbidden", "the Host header must be 127.0.0.1:" + port);
        return httplib::Server::HandlerResponse::Handled;
    }
    if (req.has_header("Origin")) {
        const std::string origin = req.get_header_value("Origin");
        if (origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port) {
            sendError(res, 403, "forbidden", "requests from other sites aren't allowed");
            return httplib::Server::HandlerResponse::Handled;
        }
    }
    if (req.method == "POST" &&
        mediaType(req.get_header_value("Content-Type")) != "application/json") {
        sendError(res, 415, "unsupported_media_type", "the body must be sent as application/json");
        return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

// GET /api/v1/search?q=...: the same JSON document `search --json` prints.
// Errors found before the search starts, like a bad query, get a 4xx or 5xx
// status. After that the status is 200, and an error ends the document
// with "status":"error", as in the CLI.
void HttpServer::Impl::search(const httplib::Request& req, httplib::Response& res) {
    SearchSettings settings;
    const std::string problem = readSearchSettings(req, settings);
    if (!problem.empty()) {
        sendSearchError(res, 400, problem);
        return;
    }

    std::shared_ptr<PreparedSearch> prepared;
    try {
        prepared = std::make_shared<PreparedSearch>(service_.prepare(settings));
    } catch (const QueryError& error) {
        sendSearchError(res, 400, error.what());
        return;
    } catch (const UnknownRepoError& error) {
        sendSearchError(res, 404, error.what());
        return;
    } catch (const NoReposError& error) {
        sendSearchError(res, 404, error.what());
        return;
    } catch (const RepoIndexError& error) {
        sendSearchError(res, 500, error.what() + rebuildHint(error.repo()));
        return;
    } catch (const std::exception& error) {
        sendSearchError(res, 500, error.what());
        return;
    }

    // The results are sent as they're found, in pieces ("chunked"), so a
    // big result never has to sit in memory. httplib calls this once the
    // handler returns. It may copy the function, so the search is held by
    // a shared_ptr, which every copy shares.
    const std::function<bool()> connectionClosed = req.is_connection_closed;
    res.set_chunked_content_provider(
        "application/json",
        [this, prepared, connectionClosed](std::size_t /*offset*/, httplib::DataSink& sink) {
            streamSearch(*prepared, connectionClosed, sink);
            return true;
        });
}

void HttpServer::Impl::streamSearch(PreparedSearch& search,
                                    const std::function<bool()>& connectionClosed,
                                    httplib::DataSink& sink) {
    RunningSearch running;
    running.deadline = std::chrono::steady_clock::now() + config_.searchTimeout;
    running.connectionClosed = connectionClosed;
    running.cancelled = stopping_.load();  // the server may have stopped since the request came
    watchdog_.add(&running);

    std::ostringstream out;
    JsonResultWriter writer(out);
    bool clientGone = false;
    // Sends what the writer has written so far, once there's enough of it,
    // or all of it at the end. If the client has gone, the search stops.
    const auto send = [&](bool all) {
        if (clientGone || (!all && out.view().size() < kChunkBytes)) {
            return;
        }
        const std::string chunk = out.str();
        out.str("");
        if (!chunk.empty() && !sink.write(chunk.data(), chunk.size())) {
            clientGone = true;
            running.cancelled = true;
        }
    };

    std::string status;
    try {
        const SearchStatus result =
            SearchService::run(search, &running.cancelled,
                               {
                                   .onFile =
                                       [&](const ResultFile& file) {
                                           if (config_.beforeResultFile) {
                                               config_.beforeResultFile();
                                           }
                                           writer.file(file);
                                           send(false);
                                       },
                                   .onSkipped =
                                       [&](const std::string& repo, const std::string& message) {
                                           writer.skipped(repo, message);
                                       },
                               });
        if (running.timedOut) {
            writer.fail("the search took longer than " +
                        std::to_string(config_.searchTimeout.count()) +
                        " ms and was stopped. The files before this are what it found.");
            status = "error";
        } else {
            writer.finish(result);
            status = statusName(result);
        }
    } catch (const RepoIndexError& error) {
        writer.fail(error.what() + rebuildHint(error.repo()));
        status = "error";
    } catch (const std::exception& error) {
        writer.fail(error.what());
        status = "error";
    } catch (...) {
        writer.fail("unknown error");
        status = "error";
    }
    watchdog_.remove(&running);

    send(true);
    if (clientGone) {
        status = "disconnected";
    } else {
        sink.done();
    }
    if (config_.onSearchDone) {
        config_.onSearchDone(status);
    }
}

// GET /api/v1/repos: {"repos":[{"name":...,"source":...,"location":...,"commit":...}]}
void HttpServer::Impl::listRepos(httplib::Response& res) const {
    Json repos = Json::array();
    for (const RepoEntry& repo : loadRegistry(config_.workspace).repos) {
        repos.push_back(repoJson(repo));
    }
    sendJson(res, 200, {{"repos", repos}});
}

// POST /api/v1/repos {"source":"<folder or GitHub URL>","name":"<optional>"}
void HttpServer::Impl::addRepo(const httplib::Request& req, httplib::Response& res) const {
    const std::optional<Json> body = readBody(req, res, {"source", "name"});
    if (!body.has_value()) {
        return;
    }
    const std::optional<std::string> source = stringField(body.value(), "source", res);
    const std::optional<std::string> name = stringField(body.value(), "name", res);
    if (!source.has_value() || !name.has_value()) {
        return;
    }
    if (source->empty()) {
        sendError(res, 400, "bad_request", "source is missing: a folder or a GitHub URL");
        return;
    }
    change(res, [&] {
        RepoManager manager(config_.workspace, config_.cloner);
        const AddResult result = manager.add(source.value(), name.value());
        sendJson(res, 201,
                 {{"repo", repoJson(result.repo)}, {"report", reportJson(result.report)}});
    });
}

// POST /api/v1/repos/<name>/update {"rebuild":false}
void HttpServer::Impl::updateRepo(const httplib::Request& req, httplib::Response& res) const {
    const std::optional<Json> body = readBody(req, res, {"rebuild"});
    if (!body.has_value()) {
        return;
    }
    bool rebuild = false;
    if (body->contains("rebuild")) {
        if (!(*body)["rebuild"].is_boolean()) {
            sendError(res, 400, "bad_request", "rebuild must be true or false");
            return;
        }
        rebuild = (*body)["rebuild"].get<bool>();
    }
    const std::string name = req.path_params.at("name");
    change(res, [&] {
        RepoManager manager(config_.workspace, config_.cloner);
        const UpdateResult result = manager.update(name, rebuild);
        Json answer = {
            {"repo", repoJson(result.repo)},
            {"rebuilt", !result.reason.empty()},
            {"reason", result.reason},
        };
        if (!result.reason.empty()) {
            answer["report"] = reportJson(result.report);
        }
        sendJson(res, 200, answer);
    });
}

// DELETE /api/v1/repos/<name>
void HttpServer::Impl::removeRepo(const httplib::Request& req, httplib::Response& res) const {
    const std::string name = req.path_params.at("name");
    change(res, [&] {
        RepoManager manager(config_.workspace, config_.cloner);
        const RepoEntry removed = manager.remove(name);
        sendJson(res, 200, {{"repo", repoJson(removed)}});
    });
}

// ---------------------------------------------------------------------------

HttpServer::HttpServer(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

HttpServer::~HttpServer() = default;

int HttpServer::bind(int port) {
    return impl_->bind(port);
}

void HttpServer::run() {
    impl_->run();
}

void HttpServer::stop() {
    impl_->stop();
}

}  // namespace codesearch
