#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include "codesearch/app/repo_manager.hpp"
#include "codesearch/repo/workspace.hpp"

namespace codesearch {

struct ServerConfig {
    Workspace workspace;
    Cloner cloner;  // clones GitHub repos for "add"
    // The built web UI (web/dist). If it has no index.html, "/" shows a
    // page saying how to build it.
    std::filesystem::path uiDir;
    // A search that runs longer than this is stopped, and its document
    // ends with "status":"error".
    std::chrono::milliseconds searchTimeout{30'000};
    std::size_t maxBodyBytes = std::size_t{64} << 10U;  // 64 KiB; bigger bodies get 413
    std::size_t maxOpenIndexes = 64;                    // index files kept open between searches
    std::size_t requestThreads = 8;                     // requests answered at once
    std::size_t maxQueuedRequests = 64;                 // more waiting than this get turned away

    // For tests. Called on the request's thread before each result file of
    // a search is sent.
    std::function<void()> beforeResultFile;
    // For tests. Called when a search's document ends, with its status, or
    // "disconnected" if the client went away first.
    std::function<void(std::string_view status)> onSearchDone;
};

// The HTTP API and the web UI, on 127.0.0.1 only. See "HTTP API" in
// docs/ARCHITECTURE.md for the routes.
class HttpServer {
public:
    explicit HttpServer(ServerConfig config);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    // Starts listening on 127.0.0.1:`port`, or a free port if `port` is 0.
    // Returns the port. Throws std::runtime_error if it can't.
    int bind(int port);

    // Answers requests until stop() is called. Call bind() first.
    void run();

    // Makes run() return, after cancelling searches still running. Safe to
    // call from another thread.
    void stop();

private:
    // The real work is in http_server.cpp, so only that file includes
    // cpp-httplib, which is big and slow to compile.
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace codesearch
