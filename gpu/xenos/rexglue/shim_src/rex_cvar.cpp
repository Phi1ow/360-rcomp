// R-comp implementation of the part of rexglue's cvar API (include/rex/cvar.h)
// the GPU layers link against (owner: gpu/xenos). Flags keep their compiled
// defaults; REX_<NAME> (upper case) in the environment overrides a flag when it
// registers, through the flag's own setter (the same rule rexglue documents
// for ApplyEnvironment). No command line or TOML config (the title has
// neither).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

namespace rex::cvar {

namespace {
std::mutex& mu() {
    static std::mutex m;
    return m;
}
FlagEntry* find(std::string_view name) {
    for (auto& e : GetRegistry())
        if (e.name == name) return &e;
    return nullptr;
}
}  // namespace

std::vector<FlagEntry>& GetRegistry() {
    static std::vector<FlagEntry> registry;
    return registry;
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
    std::lock_guard<std::mutex> lk(mu());
    if (find(entry.name)) {
        fprintf(stderr, "rex cvar: duplicate flag %s ignored\n", entry.name.c_str());
        return std::nullopt;
    }
    std::string env = "REX_";
    for (char c : entry.name) env += char(toupper((unsigned char)c));
    if (const char* v = getenv(env.c_str())) {
        if (entry.setter && entry.setter(v)) entry.source = Source::kEnvironment;
        else fprintf(stderr, "rex cvar: %s=%s rejected\n", env.c_str(), v);
    }
    GetRegistry().push_back(std::move(entry));
    return GetRegistry().size() - 1;
}

void UnregisterFlag(std::string_view name) {
    std::lock_guard<std::mutex> lk(mu());
    auto& r = GetRegistry();
    for (auto it = r.begin(); it != r.end(); ++it)
        if (it->name == name) {
            r.erase(it);
            return;
        }
}

bool HasNonDefaultValue(std::string_view name) {
    std::lock_guard<std::mutex> lk(mu());
    const FlagEntry* e = find(name);
    return e && e->getter && e->getter() != e->default_value;
}

// Code-side override (R-comp defaults, e.g. synchronous shader compilation);
// false if the flag is unknown or rejects the value.
bool SetFlagByName(std::string_view name, std::string_view value) {
    std::lock_guard<std::mutex> lk(mu());
    FlagEntry* e = find(name);
    if (!e || !e->setter || !e->setter(value)) return false;
    e->source = Source::kRuntime;
    return true;
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
    if (owned_name_.empty()) return;
    std::lock_guard<std::mutex> lk(mu());
    if (FlagEntry* e = find(owned_name_)) fn(*e);
}

}  // namespace rex::cvar
