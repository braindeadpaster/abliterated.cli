//
// Minimal JSON for the Claude stream: parse one NDJSON line into a tree, read
// fields out of it, and escape strings for the lines we write back.
//
// Deliberately small — no dependency to vendor — and forgiving: anything it
// can't parse comes back as Null rather than throwing.

#pragma once
#include <string>
#include <vector>
#include <utility>
#include <stdlib.h>
#include <stdio.h>

namespace json
{
    struct Value
    {
        enum Type { Null, Bool, Number, String, Array, Object };
        Type type = Null;
        bool b = false;
        double n = 0.0;
        std::string s;
        std::vector<Value> arr;
        std::vector<std::pair<std::string, Value>> obj;

        bool IsNull() const   { return type == Null; }
        bool IsString() const { return type == String; }
        bool IsArray() const  { return type == Array; }
        bool IsObject() const { return type == Object; }

        // Field lookup; a missing key, or a non-object, gives a shared Null.
        const Value& operator[](const char* key) const
        {
            static const Value none;
            if (type != Object) return none;
            for (const auto& kv : obj)
                if (kv.first == key) return kv.second;
            return none;
        }
        const Value& operator[](size_t i) const
        {
            static const Value none;
            return (type == Array && i < arr.size()) ? arr[i] : none;
        }
        size_t Size() const { return type == Array ? arr.size() : type == Object ? obj.size() : 0; }

        const std::string& Str() const { static const std::string e; return type == String ? s : e; }
        double Num(double def = 0.0) const { return type == Number ? n : def; }
        bool Truthy() const { return type == Bool ? b : type == Number ? n != 0.0 : false; }
    };

    namespace detail
    {
        struct Parser
        {
            const char* p;
            const char* end;

            void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

            static void Utf8(std::string& out, unsigned cp)
            {
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            }

            bool Hex4(unsigned& v)
            {
                if (end - p < 4) return false;
                v = 0;
                for (int i = 0; i < 4; i++)
                {
                    char c = *p++;
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= c - '0';
                    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                    else return false;
                }
                return true;
            }

            bool Str(std::string& out)
            {
                if (p >= end || *p != '"') return false;
                p++;
                while (p < end && *p != '"')
                {
                    char c = *p++;
                    if (c != '\\') { out += c; continue; }
                    if (p >= end) return false;
                    char e = *p++;
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        unsigned cp;
                        if (!Hex4(cp)) return false;
                        // Surrogate pair -> one codepoint.
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                        {
                            p += 2;
                            unsigned lo;
                            if (!Hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        Utf8(out, cp);
                        break;
                    }
                    default: out += e; break;
                    }
                }
                if (p >= end) return false;
                p++;
                return true;
            }

            bool Val(Value& v, int depth)
            {
                if (depth > 128) return false;
                Ws();
                if (p >= end) return false;
                char c = *p;
                if (c == '{')
                {
                    v.type = Value::Object; p++; Ws();
                    if (p < end && *p == '}') { p++; return true; }
                    for (;;)
                    {
                        Ws();
                        std::string k;
                        if (!Str(k)) return false;
                        Ws();
                        if (p >= end || *p != ':') return false;
                        p++;
                        v.obj.emplace_back(std::move(k), Value());
                        if (!Val(v.obj.back().second, depth + 1)) return false;
                        Ws();
                        if (p < end && *p == ',') { p++; continue; }
                        if (p < end && *p == '}') { p++; return true; }
                        return false;
                    }
                }
                if (c == '[')
                {
                    v.type = Value::Array; p++; Ws();
                    if (p < end && *p == ']') { p++; return true; }
                    for (;;)
                    {
                        v.arr.emplace_back();
                        if (!Val(v.arr.back(), depth + 1)) return false;
                        Ws();
                        if (p < end && *p == ',') { p++; continue; }
                        if (p < end && *p == ']') { p++; return true; }
                        return false;
                    }
                }
                if (c == '"') { v.type = Value::String; return Str(v.s); }
                if (c == 't' && end - p >= 4) { v.type = Value::Bool; v.b = true; p += 4; return true; }
                if (c == 'f' && end - p >= 5) { v.type = Value::Bool; v.b = false; p += 5; return true; }
                if (c == 'n' && end - p >= 4) { v.type = Value::Null; p += 4; return true; }
                char* stop = nullptr;
                v.n = strtod(p, &stop);
                if (stop == p) return false;
                v.type = Value::Number;
                p = stop;
                return true;
            }
        };
    }

    // Parse one document. On any syntax error the result is Null.
    inline Value Parse(const std::string& text)
    {
        Value v;
        detail::Parser ps{ text.data(), text.data() + text.size() };
        if (!ps.Val(v, 0)) return Value();
        return v;
    }

    // `s` as a JSON string literal, quotes included.
    inline std::string Quote(const std::string& s)
    {
        std::string o;
        o.reserve(s.size() + 2);
        o += '"';
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
                else o += (char)c;
            }
        }
        o += '"';
        return o;
    }

    // Serialise a parsed value back to compact JSON (used to echo a tool's input
    // back as `updatedInput` exactly as it arrived).
    inline void Write(const Value& v, std::string& o)
    {
        switch (v.type)
        {
        case Value::Null:   o += "null"; break;
        case Value::Bool:   o += v.b ? "true" : "false"; break;
        case Value::Number: { char buf[64]; snprintf(buf, sizeof(buf), "%.17g", v.n); o += buf; break; }
        case Value::String: o += Quote(v.s); break;
        case Value::Array:
            o += '[';
            for (size_t i = 0; i < v.arr.size(); i++) { if (i) o += ','; Write(v.arr[i], o); }
            o += ']';
            break;
        case Value::Object:
            o += '{';
            for (size_t i = 0; i < v.obj.size(); i++)
            {
                if (i) o += ',';
                o += Quote(v.obj[i].first);
                o += ':';
                Write(v.obj[i].second, o);
            }
            o += '}';
            break;
        }
    }
}
