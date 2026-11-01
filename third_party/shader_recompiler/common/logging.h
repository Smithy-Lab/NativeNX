// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux: the recompiler's logging, without yuzu's log backend. Messages go to stderr when NXL_SHADER_LOG is set
// (critical ones always), formatted with fmt as in yuzu.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/common_types.h"

#if FMT_VERSION >= 80100
template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_enum_v<T>, char>> : formatter<std::underlying_type_t<T>> {
    template <typename F> auto format(const T& value, F& ctx) const -> decltype(ctx.out()) {
        return fmt::formatter<std::underlying_type_t<T>>::format(std::underlying_type_t<T>(value), ctx);
    }
};
#endif

namespace Common::Log {
enum class Level : u8 { Trace, Debug, Info, Warning, Error, Critical, Count };
inline bool Enabled(Level l) {
    static const int on = std::getenv("NXL_SHADER_LOG") != nullptr;
    return l == Level::Critical || (on && l >= Level::Info);
}
template <typename... Args>
inline void FmtLogMessage(Level level, const char* file, unsigned line, fmt::format_string<Args...> format, Args&&... args) {
    if (!Enabled(level)) return;
    std::fprintf(stderr, "[shader] %s:%u: %s\n", file, line, fmt::format(format, std::forward<Args>(args)...).c_str());
}
} // namespace Common::Log

#define NXL_SHLOG(level, ...) ::Common::Log::FmtLogMessage(::Common::Log::Level::level, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_TRACE(cls, ...) NXL_SHLOG(Trace, __VA_ARGS__)
#define LOG_DEBUG(cls, ...) NXL_SHLOG(Debug, __VA_ARGS__)
#define LOG_INFO(cls, ...) NXL_SHLOG(Info, __VA_ARGS__)
#define LOG_WARNING(cls, ...) NXL_SHLOG(Warning, __VA_ARGS__)
#define LOG_ERROR(cls, ...) NXL_SHLOG(Error, __VA_ARGS__)
#define LOG_CRITICAL(cls, ...) NXL_SHLOG(Critical, __VA_ARGS__)
