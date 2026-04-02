#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yandex {

struct Config {
    std::string base_url = "https://yandex.ru/internet";
    std::string user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    std::chrono::milliseconds timeout{30000};
    std::string language = "en";
    int concurrency = 4;
};

struct ISPInfo {
    std::string name;
    int asn = 0;
};

struct Probe {
    std::string url;
    int timeout = 0;
};

struct ProbesResponse {
    std::vector<Probe> latency;
    std::vector<Probe> download;
    std::vector<std::string> upload_urls;
};

struct SpeedResult {
    double download_mbps = 0.0;
    double upload_mbps = 0.0;
    std::chrono::milliseconds latency{0};
};

struct ProgressReport {
    std::int64_t bytes = 0;
    bool is_download = true;
};

using ProgressFunc = std::function<void(const ProgressReport&)>;

class OperationContext {
public:
    explicit OperationContext(std::chrono::milliseconds timeout);

    bool cancelled() const;
    void cancel();
    std::chrono::milliseconds remaining() const;

private:
    std::chrono::steady_clock::time_point deadline_;
    std::atomic<bool> cancelled_{false};
};

class Client {
public:
    explicit Client(Config config);

    std::string GetIPv4(OperationContext& ctx) const;
    std::string GetIPv6(OperationContext& ctx) const;
    std::string GetRegion(OperationContext& ctx) const;
    std::optional<ISPInfo> GetISP(OperationContext& ctx) const;
    ProbesResponse GetProbes(OperationContext& ctx) const;
    SpeedResult RunSpeedTest(OperationContext& ctx, ProgressFunc progress) const;

private:
    std::string HttpGet(OperationContext& ctx, const std::string& url, const std::vector<std::string>& headers = {}) const;
    long HttpDownload(OperationContext& ctx, const std::string& url, std::int64_t& bytes, const std::function<void(std::size_t)>& on_chunk, const std::vector<std::string>& headers = {}) const;
    long HttpUploadZeros(OperationContext& ctx, const std::string& url, std::size_t total_size, std::int64_t& sent, const std::function<void(std::size_t)>& on_chunk, const std::vector<std::string>& headers = {}) const;

    std::chrono::milliseconds MeasureLatency(OperationContext& ctx, const std::vector<Probe>& probes) const;
    double MeasureDownloadParallel(OperationContext& ctx, const std::string& url, int concurrency, ProgressFunc progress) const;
    double MeasureUploadParallel(OperationContext& ctx, const std::string& url, int size, int concurrency, ProgressFunc progress) const;

    Config config_;
};

} // namespace yandex
