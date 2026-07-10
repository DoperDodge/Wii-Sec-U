// Shared helpers for the Wii-Sec-U Aroma plugins (wsu-input, wsu-stream).
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wsu {

// ------------------------------------------------------------------
// Logging (Cafe OS system log + WHB UDP log, viewable with udplogserver)
// ------------------------------------------------------------------

#define WSU_LOG(FMT, ...) WHBLogPrintf("[wsu] " FMT, ##__VA_ARGS__)

inline void logInit() {
    WHBLogCafeInit();
    WHBLogUdpInit();
}

// ------------------------------------------------------------------
// Monotonic milliseconds since boot.
// ------------------------------------------------------------------

inline uint32_t nowMs() {
    return static_cast<uint32_t>(OSTicksToMilliseconds(OSGetSystemTime()));
}

inline uint32_t ageMs(uint32_t now, uint32_t then) { return now - then; }

// ------------------------------------------------------------------
// Non-blocking UDP socket bound to a port. Cafe OS supports the
// WUT-specific SO_NONBLOCK socket option; the receive loops poll with a
// short OSSleepTicks instead of relying on SO_RCVTIMEO.
// ------------------------------------------------------------------

inline int openUdpSocket(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

inline void sleepMs(uint32_t ms) {
    OSSleepTicks(OSMillisecondsToTicks(ms));
}

// ------------------------------------------------------------------
// Tiny "key=value" config file reader for sd:/wiiu/ configs. Lines
// starting with '#' are comments. Missing file leaves defaults untouched.
// ------------------------------------------------------------------

class ConfigFile {
  public:
    explicit ConfigFile(const char *path) {
        FILE *f = fopen(path, "r");
        if (f == nullptr) return;
        loaded_ = true;
        char line[128];
        while (count_ < kMaxEntries && fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || eq == nullptr) continue;
            *eq = '\0';
            // strip trailing newline/space from the value
            char *val = eq + 1;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r' ||
                               val[len - 1] == ' ')) {
                val[--len] = '\0';
            }
            snprintf(keys_[count_], sizeof(keys_[count_]), "%s", line);
            snprintf(values_[count_], sizeof(values_[count_]), "%s", val);
            count_++;
        }
        fclose(f);
    }

    bool loaded() const { return loaded_; }

    int getInt(const char *key, int fallback) const {
        for (int i = 0; i < count_; i++) {
            if (strcmp(keys_[i], key) == 0) return atoi(values_[i]);
        }
        return fallback;
    }

  private:
    static constexpr int kMaxEntries = 16;
    char keys_[kMaxEntries][32] = {};
    char values_[kMaxEntries][64] = {};
    int count_ = 0;
    bool loaded_ = false;
};

// ------------------------------------------------------------------
// Background thread helper (heap-allocated stack, chosen core/priority).
// ------------------------------------------------------------------

class PluginThread {
  public:
    using Entry = int (*)(int, const char **);

    // Returns false if the thread could not be created. `priority`:
    // lower value = higher priority; Cafe OS app default is 16.
    bool start(Entry entry, uint32_t stackSize, int32_t priority,
               OSThreadAttributes affinity, const char *name) {
        if (running_) return true;
        stack_ = new (std::nothrow) uint8_t[stackSize];
        if (stack_ == nullptr) return false;
        memset(&thread_, 0, sizeof(thread_));
        if (!OSCreateThread(&thread_, entry, 0, nullptr,
                            stack_ + stackSize, stackSize, priority,
                            affinity)) {
            delete[] stack_;
            stack_ = nullptr;
            return false;
        }
        OSSetThreadName(&thread_, name);
        OSResumeThread(&thread_);
        running_ = true;
        return true;
    }

    // Joins the thread; the caller must have told the entry function to
    // exit (e.g. via an atomic flag) before calling this.
    void join() {
        if (!running_) return;
        int result = 0;
        OSJoinThread(&thread_, &result);
        delete[] stack_;
        stack_ = nullptr;
        running_ = false;
    }

    bool running() const { return running_; }

  private:
    OSThread thread_{};
    uint8_t *stack_ = nullptr;
    bool running_ = false;
};

} // namespace wsu
