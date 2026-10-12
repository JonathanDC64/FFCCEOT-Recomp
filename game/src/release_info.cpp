#include "release_info.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace update {

namespace {

// A small JSON reader: just enough for the releases API (objects, arrays, strings, numbers, booleans, null).
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;  // a string's value
    std::vector<Json> items;
    std::map<std::string, Json> fields;
};

class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}
    bool read(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return pos_ == s_.size();
    }

private:
    void space() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }
    bool literal(const char* word) {
        const std::string w = word;
        if (s_.compare(pos_, w.size(), w) != 0) return false;
        pos_ += w.size();
        return true;
    }
    static void put_utf8(std::string& out, unsigned c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        pos_++;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned u;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {  // a surrogate pair
                    pos_ += 2;
                    unsigned low;
                    if (!hex4(low)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                }
                put_utf8(out, u);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& out, int depth) {
        if (depth > 32) return false;
        space();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            out.type = Json::Type::Object;
            pos_++;
            space();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                pos_++;
                return true;
            }
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                space();
                if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
                if (!value(out.fields[key], depth + 1)) return false;
                space();
                if (pos_ >= s_.size()) return false;
                const char n = s_[pos_++];
                if (n == '}') return true;
                if (n != ',') return false;
            }
        }
        if (c == '[') {
            out.type = Json::Type::Array;
            pos_++;
            space();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                pos_++;
                return true;
            }
            for (;;) {
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
                space();
                if (pos_ >= s_.size()) return false;
                const char n = s_[pos_++];
                if (n == ']') return true;
                if (n != ',') return false;
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.type = Json::Type::Bool;
            return true;
        }
        if (literal("null")) return true;
        const size_t start = pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || std::strchr("+-.eE", s_[pos_]))) pos_++;
        if (pos_ == start) return false;
        out.type = Json::Type::Number;
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

const std::string& string_field(const Json& o, const char* key) {
    static const std::string none;
    const auto it = o.fields.find(key);
    return it != o.fields.end() && it->second.type == Json::Type::String ? it->second.text : none;
}

struct Version {
    int major = 0, minor = 0, patch = 0;
    int stage = 3;  // alpha 0, beta 1, rc 2, a release 3
};

bool parse_version(std::string s, Version& v) {
    s = s.substr(0, s.find(' '));
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    const char* p = s.c_str();
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    for (int i = 0; i < 3; i++) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
        char* end;
        *parts[i] = int(std::strtol(p, &end, 10));
        p = end;
        if (i < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p == '-') {
        const std::string tail(p + 1);
        if (tail.rfind("alpha", 0) == 0) v.stage = 0;
        else if (tail.rfind("beta", 0) == 0) v.stage = 1;
        else if (tail.rfind("rc", 0) == 0) v.stage = 2;
        else v.stage = 0;  // an unknown suffix counts as before the release
    } else if (*p != '\0') {
        return false;
    }
    return true;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

// "[text](url)" becomes "text", "![alt](url)" becomes nothing.
std::string strip_links(const std::string& line) {
    std::string out;
    for (size_t i = 0; i < line.size(); i++) {
        const bool image = line[i] == '!' && i + 1 < line.size() && line[i + 1] == '[';
        if (line[i] == '[' || image) {
            const size_t open = image ? i + 1 : i;
            const size_t close = line.find("](", open);
            const size_t end = close == std::string::npos ? close : line.find(')', close);
            if (end != std::string::npos) {
                if (!image) out += line.substr(open + 1, close - open - 1);
                i = end;
                continue;
            }
        }
        out += line[i];
    }
    return out;
}

}  // namespace

bool parse_releases(const std::string& json, std::vector<Release>& out) {
    Json root;
    if (!Reader(json).read(root) || root.type != Json::Type::Array) return false;
    for (const Json& item : root.items) {
        if (item.type != Json::Type::Object) continue;
        const auto draft = item.fields.find("draft");
        if (draft != item.fields.end() && draft->second.boolean) continue;
        Release r{string_field(item, "tag_name"), string_field(item, "html_url"), string_field(item, "body")};
        if (!r.tag.empty()) out.push_back(std::move(r));
    }
    return true;
}

bool is_newer(const std::string& candidate, const std::string& current) {
    Version a, b;
    if (!parse_version(candidate, a) || !parse_version(current, b)) return false;
    const int x[4] = {a.major, a.minor, a.patch, a.stage}, y[4] = {b.major, b.minor, b.patch, b.stage};
    return std::lexicographical_compare(std::begin(y), std::end(y), std::begin(x), std::end(x));
}

std::string display_version(const std::string& tag) {
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

std::string plain_notes(const std::string& markdown, size_t max_lines, size_t max_chars) {
    std::string out;
    size_t lines = 0;
    bool cut = false;
    size_t at = 0;
    while (at < markdown.size()) {
        size_t end = markdown.find('\n', at);
        if (end == std::string::npos) end = markdown.size();
        std::string line = markdown.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && std::isspace(static_cast<unsigned char>(line[lead]))) lead++;
        line.erase(0, lead);
        if (line.empty() || line.rfind("<!--", 0) == 0 || line.rfind("```", 0) == 0 || line.rfind("---", 0) == 0 || line[0] == '|') continue;  // (tables are not for a small panel)
        size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') hashes++;
        if (hashes > 0 && hashes < line.size() && line[hashes] == ' ') line.erase(0, hashes + 1);
        else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) line = "- " + line.substr(2);
        line = strip_links(line);
        line = replace_all(replace_all(replace_all(line, "**", ""), "__", ""), "`", "");
        if (line.empty()) continue;
        if (lines == max_lines) {
            cut = true;
            break;
        }
        if (!out.empty()) out += '\n';
        out += line;
        lines++;
    }
    if (out.size() > max_chars) {
        out.resize(max_chars);
        // Not in the middle of a UTF-8 character.
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
        // Back to the end of a word, if one is near.
        const size_t space = out.find_last_of(" \n");
        if (space != std::string::npos && out.size() - space <= 20) out.resize(space);
        cut = true;
    }
    if (cut) out += "...";
    return out;
}

}  // namespace update
