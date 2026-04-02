#include "tui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <conio.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace yandex {

namespace {

std::atomic<bool> g_interrupted{false};

void SignalHandler(int) {
    g_interrupted.store(true);
}

#ifndef _WIN32
class RawModeGuard {
public:
    RawModeGuard() {
        if (tcgetattr(STDIN_FILENO, &old_) == 0) {
            active_ = true;
            termios raw = old_;
            raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
    }

    ~RawModeGuard() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old_);
        }
    }

private:
    termios old_{};
    bool active_ = false;
};
#endif

bool QuitRequested() {
#ifdef _WIN32
    if (_kbhit()) {
        const int ch = _getch();
        return ch == 'q' || ch == 'Q' || ch == 3;
    }
    return false;
#else
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) > 0) {
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return c == 'q' || c == 'Q' || c == 3;
        }
    }
    return false;
#endif
}

std::string RenderBar(double percent, int width = 40) {
    percent = std::max(0.0, std::min(1.0, percent));
    const int filled = static_cast<int>(percent * width);

    std::string bar;
    bar.reserve(static_cast<std::size_t>(width + 2));
    bar.push_back('[');
    for (int i = 0; i < width; ++i) {
        bar.push_back(i < filled ? '#' : '-');
    }
    bar.push_back(']');
    return bar;
}

void ClearScreen() {
    std::cout << "\x1b[2J\x1b[H";
}

} // namespace

int RunTUI(const Client& client, const Config& config) {
    std::signal(SIGINT, SignalHandler);

#ifndef _WIN32
    RawModeGuard raw_mode;
#endif

    OperationContext info_ctx(config.timeout);
    std::string ipv4;
    std::string ipv6;
    std::string region;
    std::string isp;

    try {
        ipv4 = client.GetIPv4(info_ctx);
    } catch (...) {
        ipv4 = "";
    }

    try {
        ipv6 = client.GetIPv6(info_ctx);
    } catch (...) {
        ipv6 = "";
    }

    try {
        region = client.GetRegion(info_ctx);
    } catch (...) {
        region = "Unknown";
    }

    try {
        auto isp_info = client.GetISP(info_ctx);
        if (isp_info) {
            isp = isp_info->name;
        }
    } catch (...) {
        isp = "";
    }

    std::mutex mu;
    std::string phase = "download";
    auto phase_start = std::chrono::steady_clock::now();
    double current_mbps = 0.0;

    OperationContext speed_ctx(config.timeout);

    auto worker = std::async(std::launch::async, [&]() {
        return client.RunSpeedTest(speed_ctx, [&](const ProgressReport& p) {
            std::lock_guard<std::mutex> lock(mu);
            const std::string new_phase = p.is_download ? "download" : "upload";
            if (new_phase != phase) {
                phase = new_phase;
                phase_start = std::chrono::steady_clock::now();
            }

            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_start).count();
            if (elapsed > 0.01) {
                current_mbps = (static_cast<double>(p.bytes) * 8.0) / (elapsed * 1000000.0);
            }
        });
    });

    while (worker.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (g_interrupted.load() || QuitRequested()) {
            speed_ctx.cancel();
            break;
        }

        std::string local_phase;
        double local_mbps = 0.0;
        double percent = 0.0;

        {
            std::lock_guard<std::mutex> lock(mu);
            local_phase = phase;
            local_mbps = current_mbps;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_start).count();
            percent = elapsed / 8.0;
        }

        ClearScreen();
        std::cout << "Yandex Internetometer CLI\n\n";
        std::cout << "IPv4:   " << ipv4 << "\n";
        if (!ipv6.empty()) {
            std::cout << "IPv6:   " << ipv6 << "\n";
        }
        std::cout << "Region: " << region << "\n";
        std::cout << "ISP:    " << isp << "\n\n";

        if (local_phase == "download") {
            std::cout << "Measuring Download: " << std::fixed << std::setprecision(2) << local_mbps << " Mbps\n";
            std::cout << RenderBar(percent) << "\n";
        } else {
            std::cout << "Measuring Upload:   " << std::fixed << std::setprecision(2) << local_mbps << " Mbps\n";
            std::cout << RenderBar(percent) << "\n";
        }

        std::cout << "\nPress q or Ctrl+C to quit\n";
        std::cout.flush();
    }

    SpeedResult result;
    try {
        result = worker.get();
    } catch (const std::exception& ex) {
        ClearScreen();
        std::cerr << "TUI error: " << ex.what() << "\n";
        return 1;
    }

    ClearScreen();
    std::cout << "Yandex Internetometer CLI\n\n";
    std::cout << "IPv4:   " << ipv4 << "\n";
    if (!ipv6.empty()) {
        std::cout << "IPv6:   " << ipv6 << "\n";
    }
    std::cout << "Region: " << region << "\n";
    std::cout << "ISP:    " << isp << "\n\n";
    std::cout << "Results:\n";
    std::cout << "Download: " << std::fixed << std::setprecision(2) << result.download_mbps << " Mbps\n";
    std::cout << "Upload:   " << std::fixed << std::setprecision(2) << result.upload_mbps << " Mbps\n";
    std::cout << "Latency:  " << result.latency.count() << " ms\n\n";
    std::cout << "Press q or Ctrl+C to quit\n";

    return 0;
}

} // namespace yandex
