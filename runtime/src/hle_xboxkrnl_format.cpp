// Independently implemented Xbox PPC formatting using explicit guest arguments.
// ABI evidence, bounded compatibility domain and oracles: docs/FORMAT.md.
#include "physical_window.h"
#include "rcomp/guest_write_tracking.h"
#include "rcomp/runtime/format.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/strings.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"

#include <algorithm>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace rcomp::rt {
namespace {
constexpr size_t kFormatLimit = 65536;
constexpr size_t kOutputLimit = 1024 * 1024;
enum class Error { None, Access, Unsupported, Resource, Host };
struct Result {
    Error error = Error::None;
    uint32_t detail = 0;
    int32_t value = 0;
};
enum class Mode { Sprintf, Vsnprintf, Debug, Snprintf };

bool readable(GuestMemory& memory, uint64_t address, uint64_t size) {
    return address != 0 && address < (1ull << 32) && size <= (1ull << 32) - address &&
           memory.is_accessible(address, size, Protect::Read);
}

bool load64(GuestMemory& memory, uint64_t address, uint64_t& value) {
    if (!readable(memory, address, 8)) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | memory.base()[address + i];
    return true;
}

struct Arguments {
    GuestMemory& memory;
    const uint64_t registers[8];
    uint64_t stack;
    uint64_t array;
    unsigned index;
    bool is_array;
    Result failure{};

    Arguments(GuestMemory& mem, const PPCContext& context, Mode mode)
        : memory(mem), registers{context.r3.u64, context.r4.u64, context.r5.u64, context.r6.u64,
                                  context.r7.u64, context.r8.u64, context.r9.u64, context.r10.u64},
          stack(context.r1.u32), array(context.r6.u32),
          index(mode == Mode::Sprintf ? 2u : mode == Mode::Debug ? 1u : mode == Mode::Snprintf ? 3u : 0u),
          is_array(mode == Mode::Vsnprintf) {}

    bool next(uint64_t& value) {
        if (index > kOutputLimit / 2) {
            failure = {Error::Resource, index, 0};
            return false;
        }
        if (!is_array && index < 8) {
            value = registers[index++];
            return true;
        }
        // The 64-bit overflow slot starts at SP+0x50; its low BE32 word is
        // SP+0x54. Public emulator get_arg_64(+0x54) is not this game's ABI.
        const uint64_t address = is_array ? array + uint64_t(index) * 8
                                          : stack + 0x50 + uint64_t(index - 8) * 8;
        ++index;
        if (!load64(memory, address, value)) {
            failure = {Error::Access, uint32_t(address), 0};
            return false;
        }
        return true;
    }
};

bool read_string(GuestMemory& memory, uint32_t address, bool wide, size_t limit,
                 bool precision_limited, std::string& output, Result& failure) {
    if (limit == 0 && precision_limited) return true;
    if (!address) {
        output.assign("(null)", precision_limited ? std::min(limit, size_t(6)) : 6);
        return true;
    }
    const uint64_t step = wide ? 2 : 1;
    for (size_t i = 0; i < limit; ++i) {
        const uint64_t at = uint64_t(address) + i * step;
        if (!readable(memory, at, step)) {
            failure = {Error::Access, uint32_t(at), 0};
            return false;
        }
        uint16_t character = memory.base()[at];
        if (wide) character = uint16_t((character << 8) | memory.base()[at + 1]);
        if (!character) return true;
        output.push_back(char(wide ? ansi_from_unicode(character) : uint8_t(character)));
    }
    if (precision_limited) return true;
    failure = {Error::Resource, uint32_t(limit), 0};
    return false;
}

struct Spec {
    bool left = false, plus = false, space = false, alternate = false, zero = false;
    bool has_precision = false;
    unsigned width = 0, precision = 0;
    unsigned bits = 32;
    enum class Length { Default, Small, Long, Wide, Int64 } length = Length::Default;
    char conversion = 0;
};

bool parse_number(const std::string& format, size_t& offset, unsigned& value) {
    value = 0;
    while (offset < format.size() && format[offset] >= '0' && format[offset] <= '9') {
        const unsigned digit = unsigned(format[offset++] - '0');
        if (value > (kOutputLimit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool parse_spec(const std::string& format, size_t& offset, Arguments& arguments,
                Spec& spec, Result& failure) {
    for (; offset < format.size(); ++offset) {
        const char flag = format[offset];
        if (flag == '-') spec.left = true;
        else if (flag == '+') spec.plus = true;
        else if (flag == ' ') spec.space = true;
        else if (flag == '#') spec.alternate = true;
        else if (flag == '0') spec.zero = true;
        else break;
    }
    uint64_t value = 0;
    if (offset < format.size() && format[offset] == '*') {
        ++offset;
        if (!arguments.next(value)) { failure = arguments.failure; return false; }
        const uint32_t raw = uint32_t(value);
        if (raw & 0x80000000u) { spec.left = true; spec.width = 0u - raw; }
        else spec.width = raw;
        if (spec.width > kOutputLimit) { failure = {Error::Resource, spec.width, 0}; return false; }
    } else if (!parse_number(format, offset, spec.width)) {
        failure = {Error::Resource, uint32_t(offset), 0}; return false;
    }
    if (offset < format.size() && format[offset] == '.') {
        ++offset;
        spec.has_precision = true;
        if (offset < format.size() && format[offset] == '*') {
            ++offset;
            if (!arguments.next(value)) { failure = arguments.failure; return false; }
            const uint32_t raw = uint32_t(value);
            if (raw & 0x80000000u) spec.has_precision = false;
            else spec.precision = raw;
            if (spec.precision > kOutputLimit) { failure = {Error::Resource, spec.precision, 0}; return false; }
        } else if (!parse_number(format, offset, spec.precision)) {
            failure = {Error::Resource, uint32_t(offset), 0}; return false;
        }
    }
    if (format.compare(offset, 3, "I64") == 0) {
        offset += 3; spec.bits = 64; spec.length = Spec::Length::Int64;
    } else if (format.compare(offset, 3, "I32") == 0) {
        offset += 3;
    } else if (format.compare(offset, 2, "ll") == 0) {
        offset += 2; spec.bits = 64; spec.length = Spec::Length::Int64;
    } else if (format.compare(offset, 2, "hh") == 0) {
        // The legacy Microsoft formatter treats repeated h as short, not
        // the later C99 signed/unsigned-char modifier (native oracle case 11).
        offset += 2; spec.bits = 16; spec.length = Spec::Length::Small;
    } else if (offset < format.size()) {
        if (format[offset] == 'h') {
            ++offset; spec.bits = 16; spec.length = Spec::Length::Small;
        } else if (format[offset] == 'l') {
            ++offset; spec.length = Spec::Length::Long; // Xbox long is 32 bits.
        } else if (format[offset] == 'w') {
            ++offset; spec.length = Spec::Length::Wide;
        } else if (format[offset] == 'I') {
            ++offset; // guest pointer-sized integer, 32 bits
        }
    }
    if (offset == format.size()) { failure = {Error::Unsupported, 0, 0}; return false; }
    spec.conversion = format[offset++];
    return true;
}

bool append_field(std::string& output, const std::string& prefix, const std::string& body,
                  const Spec& spec, unsigned precision_zeroes, bool numeric,
                  Result& failure) {
    const size_t content = prefix.size() + precision_zeroes + body.size();
    const size_t total = std::max(content, size_t(spec.width));
    if (total > kOutputLimit - output.size()) {
        failure = {Error::Resource, uint32_t(total), 0};
        return false;
    }
    const size_t padding = total - content;
    const bool leading_zeroes = numeric && spec.zero && !spec.left && !spec.has_precision;
    if (!spec.left && !leading_zeroes) output.append(padding, ' ');
    output.append(prefix);
    if (leading_zeroes) output.append(padding, '0');
    output.append(precision_zeroes, '0');
    output.append(body);
    if (spec.left) output.append(padding, ' ');
    return true;
}

std::string digits(uint64_t value, unsigned radix, bool uppercase) {
    const char* alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buffer[64];
    char* end = buffer + sizeof buffer;
    char* begin = end;
    do { *--begin = alphabet[value % radix]; value /= radix; } while (value);
    return std::string(begin, end);
}

bool integer_field(std::string& output, uint64_t value, Spec spec, Result& failure) {
    const bool pointer = spec.conversion == 'p';
    const bool signed_value = spec.conversion == 'd' || spec.conversion == 'i';
    const unsigned bits = pointer ? 32 : spec.bits;
    if (bits < 64) value &= (uint64_t(1) << bits) - 1;
    std::string prefix;
    if (signed_value && (value & (uint64_t(1) << (bits - 1)))) {
        prefix = "-";
        value = bits == 64 ? 0ull - value : ((0ull - value) & ((uint64_t(1) << bits) - 1));
    } else if (signed_value && spec.plus) prefix = "+";
    else if (signed_value && spec.space) prefix = " ";
    const unsigned radix = pointer || spec.conversion == 'x' || spec.conversion == 'X' ? 16
                           : spec.conversion == 'o' ? 8 : 10;
    std::string body = digits(value, radix, pointer || spec.conversion == 'X');
    if (spec.has_precision && spec.precision == 0 && value == 0 && !pointer) body.clear();
    unsigned zeroes = 0;
    if (pointer) {
        // Microsoft %p uses exactly the hexadecimal width of the guest pointer.
        zeroes = unsigned(8 - body.size());
    } else if (spec.has_precision && spec.precision > body.size()) {
        zeroes = spec.precision - unsigned(body.size());
    }
    if (spec.alternate && radix == 16 && value && !pointer)
        prefix += spec.conversion == 'X' ? "0X" : "0x";
    if (spec.alternate && radix == 8 && (body.empty() || body.front() != '0') && zeroes == 0)
        zeroes = 1;
    return append_field(output, prefix, body, spec, zeroes, true, failure);
}

double legacy_tie_adjust(double value, const Spec& spec) {
    if (value == 0) return value;
    int places = spec.has_precision ? int(spec.precision) : 6;
    if (spec.conversion == 'e' || spec.conversion == 'E')
        places -= int(std::floor(std::log10(value)));
    else if (spec.conversion == 'g' || spec.conversion == 'G')
        places = std::max(places, 1) - 1 - int(std::floor(std::log10(value)));
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof raw);
    const unsigned exponent = unsigned((raw >> 52) & 0x7FF);
    uint64_t mantissa = raw & ((1ull << 52) - 1);
    if (exponent) mantissa |= 1ull << 52;
    const int power = exponent ? int(exponent) - 1023 - 52 : -1074;
    const int twos = __builtin_ctzll(mantissa);
    // x*10^places is an exact half-integer iff its remaining power of two
    // is -1 and division by 5^(-places), when required, is exact. This checks
    // binary64 directly instead of multiplying/rounding a floating value.
    if (power + twos + places != -1) return value;
    for (int factor = 0; factor < -places; ++factor) {
        if (mantissa % 5) return value;
        mantissa /= 5;
    }
    return std::nextafter(value, std::numeric_limits<double>::infinity());
}

bool float_field_nearest(std::string& output, uint64_t bits, Spec spec, Result& failure) {
    double value = 0;
    std::memcpy(&value, &bits, sizeof value);
    if (!std::isfinite(value) || (spec.length != Spec::Length::Default && spec.length != Spec::Length::Long)) {
        failure = {Error::Unsupported, uint32_t(spec.conversion), 0}; return false;
    }
    // Float rounding and non-finite legacy spellings vary by CRT revision.
    // This bounded finite domain is tested against the documented C-locale
    // profile. Field padding is applied after the legacy exponent conversion.
    if (spec.has_precision && spec.precision > 1024) {
        failure = {Error::Resource, spec.precision, 0}; return false;
    }
    std::string native = "%";
    if (spec.alternate) native += '#';
    if (spec.has_precision) native += "." + std::to_string(spec.precision);
    native += spec.conversion;
    const double magnitude = legacy_tie_adjust(std::fabs(value), spec);
    const int needed = std::snprintf(nullptr, 0, native.c_str(), magnitude);
    if (needed < 0 || size_t(needed) > kOutputLimit) {
        failure = {Error::Host, uint32_t(spec.conversion), 0}; return false;
    }
    std::string body(size_t(needed) + 1, '\0');
    if (std::snprintf(body.data(), body.size(), native.c_str(), magnitude) != needed) {
        failure = {Error::Host, uint32_t(spec.conversion), 0}; return false;
    }
    body.resize(size_t(needed));
    const size_t exponent = body.find_last_of("eE");
    if (exponent != std::string::npos && body.size() - exponent == 4 &&
        (body[exponent + 1] == '+' || body[exponent + 1] == '-'))
        body.insert(exponent + 2, 1, '0');
    std::string prefix;
    if (std::signbit(value)) prefix = "-";
    else if (spec.plus) prefix = "+";
    else if (spec.space) prefix = " ";
    spec.has_precision = false; // float precision does not suppress '0' width padding.
    return append_field(output, prefix, body, spec, 0, true, failure);
}

bool float_field(std::string& output, uint64_t bits, Spec spec, Result& failure) {
    const int previous = std::fegetround();
    if (previous == -1 || (previous != FE_TONEAREST && std::fesetround(FE_TONEAREST) != 0)) {
        failure = {Error::Host, uint32_t(spec.conversion), 0}; return false;
    }
    bool result = false;
#if defined(__cpp_exceptions)
    try {
#endif
        result = float_field_nearest(output, bits, spec, failure);
#if defined(__cpp_exceptions)
    } catch (...) {
        if (previous != FE_TONEAREST) std::fesetround(previous);
        throw;
    }
#endif
    if (previous != FE_TONEAREST && std::fesetround(previous) != 0) {
        failure = {Error::Host, uint32_t(spec.conversion), 0}; return false;
    }
    return result;
}

Result render(GuestMemory& memory, const PPCContext& context, Mode mode,
              uint32_t format_address, std::string& output) {
    Result failure;
    std::string format;
    if (!format_address || !read_string(memory, format_address, false, kFormatLimit, false, format, failure))
        return failure.error == Error::None ? Result{Error::Access, format_address, 0} : failure;
    Arguments arguments(memory, context, mode);
    for (size_t offset = 0; offset < format.size();) {
        if (output.size() == kOutputLimit) return {Error::Resource, uint32_t(kOutputLimit), 0};
        if (format[offset] != '%') { output.push_back(format[offset++]); continue; }
        ++offset;
        if (offset < format.size() && format[offset] == '%') { output.push_back('%'); ++offset; continue; }
        Spec spec;
        if (!parse_spec(format, offset, arguments, spec, failure)) return failure;
        if (!std::strchr("diuoxXpcsSCfeEgGF", spec.conversion) || spec.conversion == 0)
            return {Error::Unsupported, uint32_t(uint8_t(spec.conversion)), 0};
        uint64_t argument = 0;
        if (!arguments.next(argument)) return arguments.failure;
        if (std::strchr("diuoxXp", spec.conversion)) {
            if (spec.length == Spec::Length::Wide) return {Error::Unsupported, uint32_t(spec.conversion), 0};
            if (!integer_field(output, argument, spec, failure)) return failure;
        } else if (std::strchr("feEgGF", spec.conversion)) {
            if (mode == Mode::Debug) return {Error::Unsupported, uint32_t(spec.conversion), 0};
            if (!float_field(output, argument, spec, failure)) return failure;
        } else {
            if (spec.length == Spec::Length::Int64) return {Error::Unsupported, uint32_t(spec.conversion), 0};
            bool wide = spec.conversion == 'S' || spec.conversion == 'C' ||
                        spec.length == Spec::Length::Long || spec.length == Spec::Length::Wide;
            if (spec.length == Spec::Length::Small) wide = false;
            std::string body;
            if (spec.conversion == 's' || spec.conversion == 'S') {
                if (!read_string(memory, uint32_t(argument), wide,
                                 spec.has_precision ? spec.precision : kOutputLimit,
                                 spec.has_precision, body, failure)) return failure;
            } else {
                body.push_back(char(wide ? ansi_from_unicode(uint16_t(argument)) : uint8_t(argument)));
            }
            if (!append_field(output, {}, body, spec, 0, false, failure)) return failure;
        }
    }
    return {};
}

Result execute_inner(PPCContext& context, Mode mode) {
    Runtime* owner = runtime();
    if (!owner || !owner->mem) return {Error::Host, 0, 0};
    auto& memory = *owner->mem;
    const uint32_t format_address = mode == Mode::Debug ? context.r3.u32
                                    : mode == Mode::Sprintf ? context.r4.u32 : context.r5.u32;  // _vsnprintf, _snprintf
    std::string output;
    const Result result = render(memory, context, mode, format_address, output);
    if (result.error != Error::None) return result;
    if (mode == Mode::Debug) {
        if (std::fwrite(output.data(), 1, output.size(), stderr) != output.size() || std::fflush(stderr) != 0)
            return {Error::Host, 0, 0};
        return {Error::None, 0, 0};
    }
    const uint32_t address = context.r3.u32;
    const size_t capacity = mode == Mode::Sprintf ? output.size() + 1 : context.r4.u32;
    const size_t data_bytes = std::min(capacity, output.size());
    const bool terminated = output.size() < capacity;
    const size_t written = data_bytes + size_t(terminated);
    if (written && (!address || written > (1ull << 32) - address ||
                    !memory.is_accessible(address, written, Protect::ReadWrite)))
        return {Error::Access, address, 0};
    // Snapshotting all arguments before this point supports overlapping input
    // and destination without leaking host pointers or partially written errors.
    if (data_bytes) std::memcpy(memory.host(address), output.data(), data_bytes);
    if (terminated) memory.base()[uint64_t(address) + data_bytes] = 0;
    note_title_write(address, written);
    return {Error::None, 0, output.size() > capacity ? -1 : int32_t(output.size())};
}

Result execute(PPCContext& context, Mode mode) {
#if defined(__cpp_exceptions)
    try { return execute_inner(context, mode); }
    catch (const std::bad_alloc&) { return {Error::Resource, 0, 0}; }
#else
    return execute_inner(context, mode);
#endif
}

void finish(PPCContext& context, const char* name, Result result) {
    // All std::string/temporary owners have been destroyed before a fatal hook
    // can longjmp in host tests. No mutex or C++ owner is abandoned here.
    if (result.error == Error::Access)
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "%s guest pointer 0x%08X lr=0x%08X", name, result.detail, uint32_t(context.lr));
    if (result.error == Error::Unsupported)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "%s unsupported format conversion 0x%02X lr=0x%08X", name, result.detail, uint32_t(context.lr));
    if (result.error == Error::Resource)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "%s formatting resource limit or allocation failure (%u)", name, result.detail);
    if (result.error == Error::Host)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "%s host formatting/logging failed", name);
    context.r3.s64 = result.value;
}

void Sprintf(PPCContext& context, uint8_t*) { finish(context, "sprintf", execute(context, Mode::Sprintf)); }
void Vsnprintf(PPCContext& context, uint8_t*) { finish(context, "_vsnprintf", execute(context, Mode::Vsnprintf)); }
// _snprintf(char* buffer, size_t count, const char* format, ...): _vsnprintf's
// count and truncation rules (no terminator when the text fills count, -1 when
// it does not fit) with the variable arguments of sprintf, from r6 on.
void Snprintf(PPCContext& context, uint8_t*) { finish(context, "_snprintf", execute(context, Mode::Snprintf)); }
void DbgPrint(PPCContext& context, uint8_t*) { finish(context, "DbgPrint", execute(context, Mode::Debug)); }
// DbgBreakPoint(): a breakpoint exception. With no debugger attached (a retail
// console, and R-comp) it is an unhandled guest exception that ends the title.
void DbgBreakPoint(PPCContext& context, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP, "DbgBreakPoint lr=0x%08X", uint32_t(context.lr));
}
void KeBugCheck(PPCContext& context, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP, "KeBugCheck code=0x%08X lr=0x%08X", context.r3.u32, uint32_t(context.lr));
}
void KeBugCheckEx(PPCContext& context, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP,
                "KeBugCheckEx code=0x%08X args=0x%08X,0x%08X,0x%08X,0x%08X lr=0x%08X",
                context.r3.u32, context.r4.u32, context.r5.u32, context.r6.u32, context.r7.u32, uint32_t(context.lr));
}
}

Status register_xboxkrnl_format_hle() {
    struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
    const Impl entries[] = {{0x0001, "DbgBreakPoint", &DbgBreakPoint}, {0x0003, "DbgPrint", &DbgPrint}, {0x0052, "KeBugCheck", &KeBugCheck},
                            {0x0053, "KeBugCheckEx", &KeBugCheckEx}, {0x013B, "sprintf", &Sprintf},
                            {0x014D, "_vsnprintf", &Vsnprintf},
                            {0x013A, "_snprintf", &Snprintf}};
    for (const auto& entry : entries) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, entry.name, &ordinal) || ordinal != entry.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, ordinal, entry.function, entry.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}
}
