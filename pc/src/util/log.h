// Minimal thread-safe leveled logger.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wsu {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Optional extra destination (e.g. the GUI's log panel). Receives the
// already-formatted line. Set once at startup, before threads spawn.
using LogSinkFn = void (*)(LogLevel level, const char *line);

namespace detail {
inline std::mutex &logMutex() {
    static std::mutex m;
    return m;
}
inline LogLevel &logThreshold() {
    static LogLevel level = LogLevel::Info;
    return level;
}
inline LogSinkFn &logSink() {
    static LogSinkFn sink = nullptr;
    return sink;
}
} // namespace detail

inline void setLogLevel(LogLevel level) { detail::logThreshold() = level; }
inline void setLogSink(LogSinkFn sink) { detail::logSink() = sink; }

inline void logv(LogLevel level, const char *tag, const char *fmt,
                 va_list args) {
    if (level < detail::logThreshold()) return;
    static const char *names[] = {"DBG", "INF", "WRN", "ERR"};
    char message[512];
    std::vsnprintf(message, sizeof(message), fmt, args);
    char line[600];
    std::snprintf(line, sizeof(line), "[%s] %s: %s",
                  names[static_cast<int>(level)], tag, message);
    std::lock_guard<std::mutex> lock(detail::logMutex());
    std::fprintf(stderr, "%s\n", line);
    if (detail::logSink() != nullptr) detail::logSink()(level, line);
}

#if defined(__GNUC__) || defined(__clang__)
#define WSU_PRINTF_ATTR __attribute__((format(printf, 2, 3)))
#else
#define WSU_PRINTF_ATTR
#endif

inline void logDebug(const char *tag, const char *fmt, ...) WSU_PRINTF_ATTR;
inline void logInfo(const char *tag, const char *fmt, ...) WSU_PRINTF_ATTR;
inline void logWarn(const char *tag, const char *fmt, ...) WSU_PRINTF_ATTR;
inline void logError(const char *tag, const char *fmt, ...) WSU_PRINTF_ATTR;

inline void logDebug(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(LogLevel::Debug, tag, fmt, args);
    va_end(args);
}
inline void logInfo(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(LogLevel::Info, tag, fmt, args);
    va_end(args);
}
inline void logWarn(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(LogLevel::Warn, tag, fmt, args);
    va_end(args);
}
inline void logError(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(LogLevel::Error, tag, fmt, args);
    va_end(args);
}

#undef WSU_PRINTF_ATTR

} // namespace wsu
