#include "yandex_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <curl/curl.h>

namespace yandex {

namespace {

constexpr auto kPhaseDuration = std::chrono::seconds(8);

class CurlGlobal {
public:
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal g_curl_global;

std::size_t WriteToString(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const auto real_size = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, real_size);
    return real_size;
}

struct DownloadContext {
    std::int64_t* total = nullptr;
    std::function<void(std::size_t)> on_chunk;
};

std::size_t DiscardWithProgress(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    (void)ptr;
    auto* ctx = static_cast<DownloadContext*>(userdata);
    const auto real_size = size * nmemb;
    if (ctx && ctx->total) {
        *ctx->total += static_cast<std::int64_t>(real_size);
        if (ctx->on_chunk) {
            ctx->on_chunk(real_size);
        }
    }
    return real_size;
}

struct UploadContext {
    std::size_t remaining = 0;
    std::int64_t* sent = nullptr;
    std::function<void(std::size_t)> on_chunk;
};

std::size_t ReadZeros(char* buffer, std::size_t size, std::size_t nitems, void* instream) {
    auto* ctx = static_cast<UploadContext*>(instream);
    const auto capacity = size * nitems;
    if (!ctx || ctx->remaining == 0) {
        return 0;
    }

    const auto chunk = std::min(capacity, ctx->remaining);
    std::memset(buffer, 0, chunk);
    ctx->remaining -= chunk;

    if (ctx->sent) {
        *ctx->sent += static_cast<std::int64_t>(chunk);
    }
    if (ctx->on_chunk) {
        ctx->on_chunk(chunk);
    }

    return chunk;
}

bool ShouldStop(OperationContext& ctx) {
    return ctx.cancelled() || ctx.remaining().count() <= 0;
}

} // namespace

OperationContext::OperationContext(std::chrono::milliseconds timeout)
    : deadline_(std::chrono::steady_clock::now() + timeout) {}

bool OperationContext::cancelled() const {
    return cancelled_.load();
}

void OperationContext::cancel() {
    cancelled_.store(true);
}

std::chrono::milliseconds OperationContext::remaining() const {
    if (cancelled()) {
        return std::chrono::milliseconds(0);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
}

Client::Client(Config config) : config_(std::move(config)) {
    if (config_.concurrency <= 0) {
        config_.concurrency = 4;
    }
}

std::string Client::HttpGet(OperationContext& ctx, const std::string& url, const std::vector<std::string>& headers) const {
    if (ShouldStop(ctx)) {
        throw std::runtime_error("operation cancelled or timed out");
    }

    auto* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to init curl");
    }

    std::string response;
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, ("User-Agent: " + config_.user_agent).c_str());
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, ctx.remaining().count())));

    const auto code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("request failed: ") + curl_easy_strerror(code));
    }
    if (http_code != 200) {
        throw std::runtime_error("bad status: " + std::to_string(http_code));
    }

    return response;
}

long Client::HttpDownload(OperationContext& ctx, const std::string& url, std::int64_t& bytes, const std::function<void(std::size_t)>& on_chunk, const std::vector<std::string>& headers) const {
    bytes = 0;
    if (ShouldStop(ctx)) {
        return CURLE_OPERATION_TIMEDOUT;
    }

    auto* curl = curl_easy_init();
    if (!curl) {
        return CURLE_FAILED_INIT;
    }

    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, ("User-Agent: " + config_.user_agent).c_str());
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }

    DownloadContext dctx{&bytes, on_chunk};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardWithProgress);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, ctx.remaining().count())));

    const auto rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return rc;
    }
    return http_code;
}

long Client::HttpUploadZeros(OperationContext& ctx, const std::string& url, std::size_t total_size, std::int64_t& sent, const std::function<void(std::size_t)>& on_chunk, const std::vector<std::string>& headers) const {
    sent = 0;
    if (ShouldStop(ctx)) {
        return CURLE_OPERATION_TIMEDOUT;
    }

    auto* curl = curl_easy_init();
    if (!curl) {
        return CURLE_FAILED_INIT;
    }

    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, ("User-Agent: " + config_.user_agent).c_str());
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }

    UploadContext uctx{total_size, &sent, on_chunk};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadZeros);
    curl_easy_setopt(curl, CURLOPT_READDATA, &uctx);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(total_size));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardWithProgress);
    std::int64_t ignored = 0;
    DownloadContext dctx{&ignored, nullptr};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, ctx.remaining().count())));

    const auto rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return rc;
    }
    return http_code;
}

std::string Client::GetIPv4(OperationContext& ctx) const {
    const auto body = HttpGet(ctx, "https://ipv4-internet.yandex.net/api/v0/ip", {"Accept: application/json"});
    return nlohmann::json::parse(body).get<std::string>();
}

std::string Client::GetIPv6(OperationContext& ctx) const {
    try {
        const auto body = HttpGet(ctx, "https://ipv6-internet.yandex.net/api/v0/ip", {"Accept: application/json"});
        return nlohmann::json::parse(body).get<std::string>();
    } catch (...) {
        // IPv6 can be unavailable.
        return "";
    }
}

std::string Client::GetRegion(OperationContext& ctx) const {
    const auto base_url = config_.language == "en" ? "https://yandex.com/internet" : "https://yandex.ru/internet";
    const auto html = HttpGet(ctx, base_url);

    static const std::regex re(R"("clientRegion":(\{[^}]*\}))");
    std::smatch m;
    if (!std::regex_search(html, m, re) || m.size() < 2) {
        return "Unknown";
    }

    try {
        const auto region_json = nlohmann::json::parse(m[1].str());
        if (region_json.contains("name") && region_json["name"].is_string()) {
            return region_json["name"].get<std::string>();
        }
    } catch (...) {
    }
    return "Unknown";
}

std::optional<ISPInfo> Client::GetISP(OperationContext& ctx) const {
    const auto html = HttpGet(ctx, "https://yandex.ru/internet");

    ISPInfo info;

    static const std::regex asn_re(R"("asn":\[(\d+)\])");
    std::smatch asn_match;
    if (std::regex_search(html, asn_match, asn_re) && asn_match.size() > 1) {
        info.asn = std::stoi(asn_match[1].str());
    }

    static const std::regex isp_re(R"###("operatorName":"([^"]*)")###");
    std::smatch isp_match;
    if (std::regex_search(html, isp_match, isp_re) && isp_match.size() > 1) {
        info.name = isp_match[1].str();
    }

    if (info.name.empty() && info.asn != 0) {
        info.name = "AS" + std::to_string(info.asn);
    }

    if (info.name.empty() && info.asn == 0) {
        return std::nullopt;
    }
    return info;
}

ProbesResponse Client::GetProbes(OperationContext& ctx) const {
    const auto body = HttpGet(ctx, "https://yandex.ru/internet/api/v0/get-probes", {"Accept: application/json"});
    const auto js = nlohmann::json::parse(body);

    ProbesResponse out;

    if (js.contains("latency") && js["latency"].contains("probes")) {
        for (const auto& p : js["latency"]["probes"]) {
            Probe probe;
            probe.url = p.value("url", "");
            probe.timeout = p.value("timeout", 0);
            if (!probe.url.empty()) {
                out.latency.push_back(std::move(probe));
            }
        }
    }

    if (js.contains("download") && js["download"].contains("probes")) {
        for (const auto& p : js["download"]["probes"]) {
            Probe probe;
            probe.url = p.value("url", "");
            probe.timeout = p.value("timeout", 0);
            if (!probe.url.empty()) {
                out.download.push_back(std::move(probe));
            }
        }
    }

    if (js.contains("upload") && js["upload"].contains("probes")) {
        for (const auto& p : js["upload"]["probes"]) {
            const auto url = p.value("url", "");
            if (!url.empty()) {
                out.upload_urls.push_back(url);
            }
        }
    }

    return out;
}

std::chrono::milliseconds Client::MeasureLatency(OperationContext& ctx, const std::vector<Probe>& probes) const {
    constexpr int kCount = 3;
    auto best = std::chrono::milliseconds(10000);
    bool found = false;

    for (int i = 0; i < kCount && !ShouldStop(ctx); ++i) {
        for (const auto& p : probes) {
            if (p.url.empty()) {
                continue;
            }

            auto* curl = curl_easy_init();
            if (!curl) {
                continue;
            }

            struct curl_slist* header_list = nullptr;
            header_list = curl_slist_append(header_list, ("User-Agent: " + config_.user_agent).c_str());
            header_list = curl_slist_append(header_list, "Referer: https://yandex.ru/internet");

            std::int64_t ignored = 0;
            DownloadContext dctx{&ignored, nullptr};

            curl_easy_setopt(curl, CURLOPT_URL, p.url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardWithProgress);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dctx);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, ctx.remaining().count())));

            const auto start = std::chrono::steady_clock::now();
            const auto rc = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);

            if (rc == CURLE_OK && http_code == 200) {
                const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                if (dur < best) {
                    best = dur;
                }
                found = true;
            }
        }
    }

    if (!found) {
        throw std::runtime_error("no successful latency probes");
    }
    return best;
}

double Client::MeasureDownloadParallel(OperationContext& ctx, const std::string& url, int concurrency, ProgressFunc progress) const {
    std::atomic<std::int64_t> total_read{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(concurrency));

    for (int i = 0; i < concurrency; ++i) {
        workers.emplace_back([&, url]() {
            while (!ShouldStop(ctx) && std::chrono::steady_clock::now() - start < kPhaseDuration) {
                std::int64_t read_this_request = 0;
                const auto status = HttpDownload(
                    ctx,
                    url,
                    read_this_request,
                    [&](std::size_t n) {
                        const auto current = total_read.fetch_add(static_cast<std::int64_t>(n)) + static_cast<std::int64_t>(n);
                        if (progress) {
                            progress(ProgressReport{current, true});
                        }
                    },
                    {"Referer: https://yandex.ru/"});

                if (status == 403) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (status == 0 || status == CURLE_OPERATION_TIMEDOUT) {
                    return;
                }
                if (read_this_request == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(total_read.load()) * 8.0) / elapsed;
}

double Client::MeasureUploadParallel(OperationContext& ctx, const std::string& url, int size, int concurrency, ProgressFunc progress) const {
    std::atomic<std::int64_t> total_sent{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(concurrency));

    for (int i = 0; i < concurrency; ++i) {
        workers.emplace_back([&, url, size]() {
            while (!ShouldStop(ctx) && std::chrono::steady_clock::now() - start < kPhaseDuration) {
                std::int64_t sent_this_request = 0;
                const auto status = HttpUploadZeros(
                    ctx,
                    url,
                    static_cast<std::size_t>(size),
                    sent_this_request,
                    [&](std::size_t n) {
                        const auto current = total_sent.fetch_add(static_cast<std::int64_t>(n)) + static_cast<std::int64_t>(n);
                        if (progress) {
                            progress(ProgressReport{current, false});
                        }
                    },
                    {
                        "Referer: https://yandex.ru/internet",
                        "Origin: https://yandex.ru",
                        "Accept: */*",
                        "Sec-Fetch-Mode: cors",
                        "Sec-Fetch-Site: cross-site",
                        "Sec-Fetch-Dest: empty",
                    });

                if (status == 0 || status == CURLE_OPERATION_TIMEDOUT) {
                    return;
                }
                if (status != 200) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(total_sent.load()) * 8.0) / elapsed;
}

SpeedResult Client::RunSpeedTest(OperationContext& ctx, ProgressFunc progress) const {
    auto probes = GetProbes(ctx);

    SpeedResult result;

    if (!probes.latency.empty()) {
        try {
            result.latency = MeasureLatency(ctx, probes.latency);
        } catch (...) {
        }
    }

    if (!probes.download.empty()) {
        std::string target = probes.download.front().url;
        for (const auto& p : probes.download) {
            if (p.url.find("100kb") == std::string::npos) {
                target = p.url;
            }
            if (p.url.find("50mb") != std::string::npos) {
                target = p.url;
                break;
            }
        }
        if (!target.empty()) {
            try {
                result.download_mbps = MeasureDownloadParallel(ctx, target, config_.concurrency, progress) / 1000000.0;
            } catch (...) {
            }
        }
    }

    if (!probes.upload_urls.empty()) {
        const auto& upload_url = probes.upload_urls.front();
        try {
            result.upload_mbps = MeasureUploadParallel(ctx, upload_url, 50 * 1024 * 1024, config_.concurrency, progress) / 1000000.0;
        } catch (...) {
        }
    }

    return result;
}

} // namespace yandex
