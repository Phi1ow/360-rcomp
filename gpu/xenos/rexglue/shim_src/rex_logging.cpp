// R-comp implementation of the part of rexglue's logging API
// (include/rex/logging/api.h) the GPU layers link against (owner: gpu/xenos):
// one spdlog logger per category writing to stderr. Level: REX_LOG_LEVEL
// (trace/debug/info/warn/error/critical/off), default warn.
#include <stdlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <rex/logging.h>

namespace rex {

namespace {
struct Registry {
    std::mutex mu;
    std::vector<LogCategoryEntry> entries;
    std::shared_ptr<spdlog::sinks::stderr_sink_mt> sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
};
Registry& reg() {
    static Registry r;
    return r;
}
spdlog::level::level_enum default_level() {
    const char* v = getenv("REX_LOG_LEVEL");
    return v ? spdlog::level::from_str(v) : spdlog::level::warn;
}
}  // namespace

LogCategoryId RegisterLogCategory(const char* name) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.mu);
    for (size_t i = 0; i < r.entries.size(); ++i)
        if (r.entries[i].name == name) return LogCategoryId(uint16_t(i));
    LogCategoryEntry e{};
    e.name = name;
    e.logger = std::make_shared<spdlog::logger>(name, r.sink);
    e.logger->set_level(default_level());
    r.entries.push_back(std::move(e));
    return LogCategoryId(uint16_t(r.entries.size() - 1));
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.mu);
    return category.id < r.entries.size() ? r.entries[category.id].logger : nullptr;
}

spdlog::logger* GetLoggerRaw(LogCategoryId category) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.mu);
    return category.id < r.entries.size() ? r.entries[category.id].logger.get() : nullptr;
}

}  // namespace rex
