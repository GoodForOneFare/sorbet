#include "WatchmanProcess.h"
#include "rapidjson/document.h"
#include "subprocess.hpp"

using namespace std;

namespace sorbet::realmain::lsp::watchman {

WatchmanProcess::WatchmanProcess(shared_ptr<spdlog::logger> logger, string_view watchmanPath, string_view workSpace,
                                 vector<string> extensions,
                                 function<void(unique_ptr<sorbet::realmain::lsp::WatchmanQueryResponse>)> processUpdate,
                                 std::function<void(int)> processExit)
    : logger(logger), watchmanPath(string(watchmanPath)), workSpace(string(workSpace)), extensions(extensions),
      processUpdate(processUpdate), processExit(processExit),
      thread(runInAThread("watchmanReader", std::bind(&WatchmanProcess::start, this))) {}

WatchmanProcess::~WatchmanProcess() {
    exitWithCode(0);
    // Destructor of Joinable ensures Watchman thread exits before this destructor finishes.
};

void WatchmanProcess::start() {
    auto mainPid = getpid();
    try {
        string subscriptionName = fmt::format("ruby-typer-{}", getpid());

        logger->debug("Starting monitoring path {} with watchman for files with extensions {}. Subscription id: {}",
                      workSpace, fmt::join(extensions, ","), subscriptionName);

        auto p = subprocess::Popen({watchmanPath.c_str(), "-j", "-p", "--no-pretty"},
                                   subprocess::output{subprocess::PIPE}, subprocess::input{subprocess::PIPE});

        // Note: Newer versions of Watchman (post 4.9.0) support ["suffix", ["suffix1", "suffix2", ...]], but Stripe
        // laptops have 4.9.0. Thus, we use [ "anyof", [ "suffix", "suffix1" ], [ "suffix", "suffix2" ], ... ].
        // Note 2: `empty_on_fresh_instance` prevents Watchman from sending entire contents of folder if this
        // subscription starts the daemon / causes the daemon to watch this folder for the first time.
        string subscribeCommand = fmt::format("[\"subscribe\", \"{}\", \"{}\", {{\n"
                                              "  \"expression\": [\"allof\", "
                                              "    [\"type\", \"f\"],\n"
                                              "    [\"anyof\", {}]"
                                              "  ],\n"
                                              "  \"defer_vcs\": false,\n"
                                              "  \"fields\": [\"name\"],\n"
                                              "  \"empty_on_fresh_instance\": true\n"
                                              "}}]\n",
                                              workSpace, subscriptionName,
                                              fmt::map_join(extensions, ",", [](const std::string &ext) -> string {
                                                  return fmt::format("[\"suffix\", \"{}\"]", ext);
                                              }));
        p.send(subscribeCommand.c_str(), subscribeCommand.size());
        logger->debug(subscribeCommand);

        auto file = p.output();
        auto fd = fileno(file);

        string buffer;

        while (!isStopped()) {
            auto maybeLine = FileOps::readLineFromFd(fd, buffer);
            if (!maybeLine) {
                // Timeout occurred. See if we should abort before reading further.
                continue;
            }

            const string &line = *maybeLine;
            // Line found!
            rapidjson::MemoryPoolAllocator<> alloc;
            rapidjson::Document d(&alloc);
            logger->debug(line);
            if (d.Parse(line.c_str()).HasParseError()) {
                logger->error("Error parsing Watchman response: `{}` is not a valid json object", line);
            } else if (d.HasMember("is_fresh_instance")) {
                try {
                    auto queryResponse = sorbet::realmain::lsp::WatchmanQueryResponse::fromJSONValue(d);
                    processUpdate(move(queryResponse));
                } catch (sorbet::realmain::lsp::DeserializationError e) {
                    // Gracefully handle deserialization errors, since they could be our fault.
                    logger->error("Unable to deserialize Watchman request: {}\nOriginal request:\n{}", e.what(), line);
                }
            } else if (!d.HasMember("subscribe")) {
                // Not a subscription response, or a file update.
                logger->debug("Unknown Watchman response:\n{}", line);
            }
        }
    } catch (exception e) {
        // Ignore exceptions thrown on forked process.
        if (getpid() == mainPid) {
            logger->info("Error running Watchman (with `{} -j -p--no-pretty`).\nWatchman is required for Sorbet to "
                         "detect changes to files made outside of your code editor.\nDon't need Watchman? Run Sorbet "
                         "with `--disable-watchman`.",
                         watchmanPath);
            exitWithCode(1);
        } else {
            // The forked process failed to start, likely because Watchman wasn't found. Exit the process.
            exit(1);
        }
    }
}

bool WatchmanProcess::isStopped() {
    absl::MutexLock lck(&mutex);
    return stopped;
}

void WatchmanProcess::exitWithCode(int code) {
    absl::MutexLock lck(&mutex);
    if (!stopped) {
        stopped = true;
        processExit(code);
    }
}

} // namespace sorbet::realmain::lsp::watchman