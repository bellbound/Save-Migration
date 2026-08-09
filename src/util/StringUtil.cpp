#include "util/StringUtil.h"

#include <algorithm>

#include <array>
#include <cctype>
#include <ctime>
#include <format>

namespace SaveMigration::Util {

namespace {

/// Windows-1252 code points for bytes 0x80-0x9F. Zero marks the five
/// unassigned slots (0x81, 0x8D, 0x8F, 0x90, 0x9D), which we map to U+FFFD.
constexpr std::array<char32_t, 32> kCp1252Upper = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void ConvertJsonStringsToUtf8(nlohmann::json& node) {
    if (node.is_string()) {
        node = ConvertSkyrimTextToUTF8(node.get<std::string>());
        return;
    }
    if (node.is_array()) {
        for (auto& element : node) {
            ConvertJsonStringsToUtf8(element);
        }
        return;
    }
    if (node.is_object()) {
        auto converted = nlohmann::json::object();
        for (auto it = node.begin(); it != node.end(); ++it) {
            nlohmann::json value = it.value();
            ConvertJsonStringsToUtf8(value);
            converted.emplace(ConvertSkyrimTextToUTF8(it.key()), std::move(value));
        }
        node = std::move(converted);
    }
}

}  // namespace

bool IsValidUtf8(std::string_view text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = p + text.size();

    while (p < end) {
        const unsigned char b = *p;
        size_t extra = 0;
        char32_t cp = 0;

        if (b < 0x80) {
            ++p;
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            extra = 1;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            extra = 2;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            extra = 3;
            cp = b & 0x07;
        } else {
            return false;  // continuation byte or 5/6-byte lead: not UTF-8
        }

        if (static_cast<size_t>(end - p) < extra + 1) {
            return false;  // truncated sequence
        }
        for (size_t i = 1; i <= extra; ++i) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }

        // Reject overlong encodings, surrogates and out-of-range code points.
        // A lax validator here would let a Windows-1252 string masquerade as
        // UTF-8 and reach dump() unconverted.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000)) {
            return false;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }

        p += extra + 1;
    }
    return true;
}

std::string ConvertSkyrimTextToUTF8(std::string_view text) {
    if (text.empty() || IsValidUtf8(text)) {
        return std::string(text);
    }

    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for (unsigned char b : text) {
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
        } else if (b < 0xA0) {
            const char32_t cp = kCp1252Upper[b - 0x80];
            AppendUtf8(out, cp ? cp : 0xFFFD);
        } else {
            AppendUtf8(out, static_cast<char32_t>(b));  // 0xA0-0xFF == Latin-1
        }
    }
    return out;
}

std::string SafeDump(const nlohmann::json& json, int indent, char indentChar, bool ensureAscii) {
    try {
        return json.dump(indent, indentChar, ensureAscii);
    } catch (const nlohmann::json::exception&) {
        nlohmann::json sanitized = json;
        ConvertJsonStringsToUtf8(sanitized);
        try {
            return sanitized.dump(indent, indentChar, ensureAscii,
                                  nlohmann::json::error_handler_t::replace);
        } catch (const std::exception& e) {
            spdlog::error("SafeDump: unrecoverable dump failure: {}", e.what());
            return "{}";
        }
    }
}

std::string PathToUtf8String(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string SanitizeForFileName(std::string_view text, size_t maxLength) {
    std::string out;
    out.reserve(std::min(text.size(), maxLength));
    for (char c : text) {
        if (out.size() >= maxLength) {
            break;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == ' ' || c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    // Windows silently drops trailing dots and spaces from directory names,
    // which would desync the path we recorded from the path that exists.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out.empty() ? std::string("Unnamed") : out;
}

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitAndTrim(std::string_view text, char delim) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t next = text.find(delim, pos);
        const auto piece = text.substr(pos, next == std::string_view::npos ? std::string_view::npos
                                                                          : next - pos);
        auto trimmed = Trim(piece);
        if (!trimmed.empty()) {
            out.push_back(std::move(trimmed));
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    return out;
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string FormatUnixMsLocal(int64_t unixMs) {
    constexpr std::string_view kUnknown = "an unknown date";
    if (unixMs <= 0) {
        return std::string(kUnknown);
    }

    const auto seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm local{};
    // localtime_s over localtime: the latter returns a pointer into a shared
    // static buffer, and this can be called from the worker thread.
    if (localtime_s(&local, &seconds) != 0) {
        return std::string(kUnknown);
    }

    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (local.tm_mon < 0 || local.tm_mon > 11) {
        return std::string(kUnknown);
    }
    return std::format("{} {} {}, {:02}:{:02}", local.tm_mday, kMonths[local.tm_mon],
                       local.tm_year + 1900, local.tm_hour, local.tm_min);
}

}  // namespace SaveMigration::Util
