/* 存储共享工具：键/命名空间校验、键 → 路径 / URL、固定协议用的极简 JSON（见 storage_internal.h）。 */
#include "storage_internal.h"

#include <cstdio>
#include <cstring>

#ifdef MQ_HAVE_CURL
#include <curl/curl.h>
#endif

namespace maque::storage {

bool segment_is_valid(const std::string& seg) {
    if (seg.empty() || seg == "." || seg == "..") return false;
    for (const unsigned char c : seg) {
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) return false;   // 分隔符 / 控制字符
    }
    return true;
}

bool key_is_valid(const std::string& key) {
    if (key.empty() || key.size() > 1024) return false;
    if (key.front() == '/') return false;                        // 绝对路径
    size_t i = 0;
    while (i <= key.size()) {
        const size_t j = key.find('/', i);
        const std::string seg = key.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!segment_is_valid(seg)) return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

bool ns_is_valid(const std::string& uid, const std::string& session) {
    return segment_is_valid(uid) && segment_is_valid(session);
}

std::string ns_of(const std::string& uid, const std::string& session) { return uid + "/" + session; }

std::string rel_path(const std::string& ns, const std::string& key) { return ns + "/" + key; }

std::string url_path(const std::string& ns, const std::string& key) {
    const std::string rel = rel_path(ns, key);
    std::string out = "/";                               // 绝对路径（base 不带尾 '/'）
    if (rel.size() > 4096) return out;
    size_t i = 0;
    while (i <= rel.size()) {
        const size_t j = rel.find('/', i);
        const std::string seg = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!seg.empty()) {
#ifdef MQ_HAVE_CURL
            char* esc = curl_easy_escape(nullptr, seg.c_str(), (int)seg.size());
            if (!esc) return std::string();
            out += esc;
            curl_free(esc);
#else
            out += seg;
#endif
        }
        if (j == std::string::npos) break;
        out.push_back('/');
        i = j + 1;
    }
    return out;
}

/* ---- 极简 JSON（固定帧自产自销）---- */

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out.push_back((char)c);      // UTF-8 原样（合法 JSON）
        }
    }
    return out;
}

std::string keys_json(const std::vector<std::string>& keys) {
    std::string out = "[";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out.push_back(',');
        out += '"' + json_escape(keys[i]) + '"';
    }
    out.push_back(']');
    return out;
}

/* ---- 极简 JSON 扫描（固定帧自产自销；不追求通用/容错）---- */

/* 跳过字符串（i 指向起始引号）；返回结束引号之后的位置。 */
static size_t skip_string(const std::string& j, size_t i) {
    ++i;
    while (i < j.size()) {
        if (j[i] == '\\') { i += 2; continue; }
        if (j[i] == '"') return i + 1;
        ++i;
    }
    return j.size();
}

/* 跳过任意值（字符串/对象/数组/标量）；返回结束后位置。 */
static size_t skip_value(const std::string& j, size_t i) {
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == '\r')) ++i;
    if (i >= j.size()) return j.size();
    if (j[i] == '"') return skip_string(j, i);
    if (j[i] == '{' || j[i] == '[') {
        int depth = 0;
        while (i < j.size()) {
            if (j[i] == '"') { i = skip_string(j, i); continue; }
            if (j[i] == '{' || j[i] == '[') { depth++; ++i; continue; }
            if (j[i] == '}' || j[i] == ']') { depth--; ++i; if (depth == 0) return i; continue; }
            ++i;
        }
        return j.size();
    }
    while (i < j.size() && j[i] != ',' && j[i] != '}') ++i;
    return i;
}

/* 字符串内容 → 原字节（\" \\ \/ \b \f \n \r \t \uXXXX） */
static std::string unescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') { out.push_back(raw[i]); continue; }
        const char e = (i + 1 < raw.size()) ? raw[++i] : '\0';
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                unsigned cp = 0;
                for (int d = 1; d <= 4 && i + (size_t)d < raw.size(); ++d) {
                    const char h = raw[i + d];
                    cp <<= 4;
                    cp |= (h >= '0' && h <= '9') ? (unsigned)(h - '0')
                        : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10)
                        : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10) : 0u;
                }
                i += 4;
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: out.push_back(e); break;
        }
    }
    return out;
}

bool json_get_str(const std::string& json, const char* field, std::string& out) {
    size_t i = json.find('{');
    if (i == std::string::npos) return false;
    ++i;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == ',' || json[i] == '\t' ||
                                   json[i] == '\n' || json[i] == '\r')) ++i;
        if (i >= json.size() || json[i] == '}') return false;
        if (json[i] != '"') return false;
        const size_t ks = i + 1;
        const size_t ke = skip_string(json, i) - 1;
        if (ke <= ks) return false;
        const std::string key = json.substr(ks, ke - ks);
        i = skip_string(json, i);
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != ':') return false;
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (key == field) {
            if (i >= json.size() || json[i] != '"') return false;      // 字段在但不是字符串
            const size_t vs = i + 1;
            const size_t ve = skip_string(json, i) - 1;
            if (ve < vs) return false;
            out = unescape(json.substr(vs, ve - vs));
            return true;
        }
        i = skip_value(json, i);
    }
    return false;
}


/* ---- base64（ws 帧传二进制值；http/file 直接传字节，无需编码）---- */

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0;
        const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kB64[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kB64[n & 0x3F] : '=');
    }
    return out;
}

bool base64_decode(const std::string& in, Blob& out) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=') break;
        if (c == '\n' || c == '\r') continue;
        const int v = value_of(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

/* 该字节串是否为合法 UTF-8（且不含 NUL）→ 决定 ws 帧用 utf8 还是 base64。 */
bool utf8_ok(const uint8_t* data, size_t size) {
    size_t i = 0;
    while (i < size) {
        const uint8_t c = data[i];
        if (c == 0) return false;
        if (c < 0x80) { ++i; continue; }
        int extra = 0;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + (size_t)extra >= size) return false;
        for (int k = 1; k <= extra; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) return false;
        }
        i += (size_t)extra + 1;
    }
    return true;
}

/* 取顶层字符串数组字段：{"keys":["a","b/c"]} → {"a","b/c"} */
bool json_get_str_array(const std::string& json, const char* field, std::vector<std::string>& out) {
    size_t i = json.find('{');
    if (i == std::string::npos) return false;
    ++i;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == ',' || json[i] == '\t' ||
                                   json[i] == '\n' || json[i] == '\r')) ++i;
        if (i >= json.size() || json[i] == '}') return false;
        if (json[i] != '"') return false;
        const size_t ks = i + 1;
        const size_t ke = skip_string(json, i) - 1;
        if (ke <= ks) return false;
        const std::string key = json.substr(ks, ke - ks);
        i = skip_string(json, i);
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != ':') return false;
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (key != field) { i = skip_value(json, i); continue; }
        if (i >= json.size() || json[i] != '[') return false;
        ++i;
        out.clear();
        while (i < json.size()) {
            while (i < json.size() && (json[i] == ' ' || json[i] == ',' || json[i] == '\t' ||
                                       json[i] == '\n' || json[i] == '\r')) ++i;
            if (i < json.size() && json[i] == ']') return true;
            if (i >= json.size() || json[i] != '"') return false;
            const size_t vs = i + 1;
            const size_t ve = skip_string(json, i) - 1;
            if (ve < vs) return false;
            out.push_back(unescape(json.substr(vs, ve - vs)));
            i = skip_string(json, i);
        }
        return false;
    }
    return false;
}

} // namespace maque::storage
