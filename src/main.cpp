#include "tui.hpp"
#include "yandex_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
    bool show_ip = false;
    bool show_speed = false;
    bool show_full = false;
    bool as_json = false;
    std::string lang = "en";
    int concurrency = 4;
    std::string save_path;
    bool prometheus = false;
    bool use_tui = false;
    std::chrono::milliseconds timeout{60000};
};

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::chrono::milliseconds ParseTimeoutValue(const std::string& value) {
    if (value.empty()) {
        return std::chrono::milliseconds(60000);
    }

    try {
        if (EndsWith(value, "ms")) {
            return std::chrono::milliseconds(static_cast<int>(std::stod(value.substr(0, value.size() - 2))));
        }
        if (EndsWith(value, "s")) {
            return std::chrono::milliseconds(static_cast<int>(std::stod(value.substr(0, value.size() - 1)) * 1000.0));
        }
        if (EndsWith(value, "m")) {
            return std::chrono::milliseconds(static_cast<int>(std::stod(value.substr(0, value.size() - 1)) * 60.0 * 1000.0));
        }
        if (EndsWith(value, "h")) {
            return std::chrono::milliseconds(static_cast<int>(std::stod(value.substr(0, value.size() - 1)) * 3600.0 * 1000.0));
        }
        return std::chrono::milliseconds(static_cast<int>(std::stod(value) * 1000.0));
    } catch (...) {
        return std::chrono::milliseconds(60000);
    }
}

Args ParseArgs(int argc, char** argv) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--ip") {
            args.show_ip = true;
        } else if (arg == "--speed") {
            args.show_speed = true;
        } else if (arg == "--all") {
            args.show_full = true;
        } else if (arg == "--json") {
            args.as_json = true;
        } else if (arg == "--prometheus") {
            args.prometheus = true;
        } else if (arg == "--tui") {
            args.use_tui = true;
        } else if (arg == "--lang" && i + 1 < argc) {
            args.lang = argv[++i];
        } else if (arg == "--concurrency" && i + 1 < argc) {
            args.concurrency = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--save" && i + 1 < argc) {
            args.save_path = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            args.timeout = ParseTimeoutValue(argv[++i]);
        } else if (StartsWith(arg, "--timeout=") ) {
            args.timeout = ParseTimeoutValue(arg.substr(std::string("--timeout=").size()));
        }
    }

    if (!args.show_ip && !args.show_speed && !args.show_full && !args.prometheus && !args.as_json) {
        args.use_tui = true;
    }

    return args;
}

std::string DetectOS() {
#ifdef _WIN32
    return "windows";
#elif __APPLE__
    return "darwin";
#elif __linux__
    return "linux";
#else
    return "unknown";
#endif
}

std::string DetectArch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "386";
#else
    return "unknown";
#endif
}

std::string NowRFC3339() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void SaveResult(const nlohmann::json& result, const std::string& path) {
    std::ofstream out(path, std::ios::app);
    if (!out) {
        std::cerr << "Failed to open log file: " << path << "\n";
        return;
    }
    out << result.dump() << "\n";
}

void PrintText(const nlohmann::json& res) {
    std::cout << "--- Yandex Internetometer CLI ---\n";

    if (res.contains("ipv4")) {
        std::cout << "IPv4: " << res["ipv4"].get<std::string>() << "\n";
    }

    if (res.contains("ipv6") && !res["ipv6"].get<std::string>().empty()) {
        std::cout << "IPv6: " << res["ipv6"].get<std::string>() << "\n";
    } else if (res.contains("ipv4")) {
        std::cout << "IPv6: -\n";
    }

    if (res.contains("region")) {
        std::cout << "Region:   " << res["region"].get<std::string>() << "\n";
    }
    if (res.contains("isp")) {
        std::cout << "ISP:      " << res["isp"].get<std::string>() << " (AS" << res.value("asn", 0) << ")\n";
    }

    if (res.contains("download_mbps")) {
        std::cout << "Download: " << std::fixed << std::setprecision(2) << res["download_mbps"].get<double>() << " Mbps\n";
    }
    if (res.contains("upload_mbps")) {
        std::cout << "Upload:   " << std::fixed << std::setprecision(2) << res["upload_mbps"].get<double>() << " Mbps\n";
    }
    if (res.contains("latency_ms")) {
        std::cout << "Latency:  " << res["latency_ms"].get<long long>() << " ms\n";
    }

    if (res.contains("os")) {
        std::cout << "OS:       " << res["os"].get<std::string>() << " (" << res.value("arch", "unknown") << ")\n";
    }
    if (res.contains("time")) {
        std::cout << "Time:     " << res["time"].get<std::string>() << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto args = ParseArgs(argc, argv);

    yandex::Config config;
    config.timeout = args.timeout;
    config.language = args.lang;
    config.concurrency = args.concurrency;

    yandex::Client client(config);

    if (args.use_tui) {
        return yandex::RunTUI(client, config);
    }

    yandex::OperationContext ctx(args.timeout);
    nlohmann::json results = nlohmann::json::object();

    if (args.show_ip || args.show_full) {
        try {
            results["ipv4"] = client.GetIPv4(ctx);
        } catch (const std::exception& ex) {
            std::cerr << "Error getting IPv4: " << ex.what() << "\n";
        }

        try {
            const auto ipv6 = client.GetIPv6(ctx);
            if (!ipv6.empty()) {
                results["ipv6"] = ipv6;
            }
        } catch (...) {
        }

        try {
            results["region"] = client.GetRegion(ctx);
        } catch (...) {
        }

        try {
            auto isp = client.GetISP(ctx);
            if (isp) {
                results["isp"] = isp->name;
                results["asn"] = isp->asn;
            }
        } catch (...) {
        }
    }

    if (args.show_full) {
        results["os"] = DetectOS();
        results["arch"] = DetectArch();
        results["num_cpu"] = static_cast<int>(std::thread::hardware_concurrency());
        results["time"] = NowRFC3339();
    }

    if (args.show_speed || args.show_full || args.prometheus) {
        if (!args.as_json && !args.prometheus) {
            std::cerr << "Running speed test...\n";
        }

        std::atomic<bool> have_phase_start{false};
        std::atomic<bool> is_download{true};
        auto phase_start = std::chrono::steady_clock::now();

        auto progress = [&](const yandex::ProgressReport& p) {
            if (args.as_json || args.prometheus) {
                return;
            }

            if (!have_phase_start.load() || p.is_download != is_download.load()) {
                is_download.store(p.is_download);
                phase_start = std::chrono::steady_clock::now();
                have_phase_start.store(true);
            }

            const auto duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_start).count();
            if (duration > 0.0) {
                const double mbps = (static_cast<double>(p.bytes) * 8.0) / (duration * 1000000.0);
                const std::string label = p.is_download ? "Download" : "Upload  ";
                std::cout << "\r" << label << ": " << std::fixed << std::setprecision(2) << mbps << " Mbps" << std::flush;
            }
        };

        try {
            const auto speed = client.RunSpeedTest(ctx, progress);
            if (!args.as_json && !args.prometheus) {
                std::cout << "\r                         \r";
            }
            results["download_mbps"] = speed.download_mbps;
            results["upload_mbps"] = speed.upload_mbps;
            results["latency_ms"] = speed.latency.count();
        } catch (const std::exception& ex) {
            std::cerr << "Speed test failed: " << ex.what() << "\n";
        }
    }

    if (args.prometheus) {
        std::string labels;
        if (results.contains("isp")) {
            labels += "isp=\"" + results["isp"].get<std::string>() + "\",";
        }
        if (results.contains("region")) {
            labels += "region=\"" + results["region"].get<std::string>() + "\",";
        }
        if (!labels.empty()) {
            labels = "{" + labels.substr(0, labels.size() - 1) + "}";
        }

        std::cout << "# HELP internetometer_download_mbps Download speed in Mbps\n";
        std::cout << "# TYPE internetometer_download_mbps gauge\n";
        std::cout << "internetometer_download_mbps" << labels << " " << std::fixed << std::setprecision(2) << results.value("download_mbps", 0.0) << "\n";

        std::cout << "# HELP internetometer_upload_mbps Upload speed in Mbps\n";
        std::cout << "# TYPE internetometer_upload_mbps gauge\n";
        std::cout << "internetometer_upload_mbps" << labels << " " << std::fixed << std::setprecision(2) << results.value("upload_mbps", 0.0) << "\n";

        std::cout << "# HELP internetometer_latency_ms Network latency in milliseconds\n";
        std::cout << "# TYPE internetometer_latency_ms gauge\n";
        std::cout << "internetometer_latency_ms" << labels << " " << results.value("latency_ms", 0LL) << "\n";
        return 0;
    }

    if (args.as_json) {
        std::cout << results.dump(2) << "\n";
    } else {
        PrintText(results);
    }

    if (!args.save_path.empty()) {
        SaveResult(results, args.save_path);
    }

    return 0;
}
