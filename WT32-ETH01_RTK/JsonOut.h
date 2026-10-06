// =============================================================
//  輕量 JSON 輸出工具 (免外部函式庫)
// =============================================================
#pragma once

#include <Arduino.h>
#include <math.h>

inline String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 2);
  out += '"';
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

class JsonOut {
public:
  explicit JsonOut(size_t reserve = 256) { _s.reserve(reserve); _s = "{"; }

  JsonOut &add(const char *k, const String &v) { key(k); _s += jsonEscape(v); return *this; }
  JsonOut &add(const char *k, const char *v)   { return add(k, String(v)); }
  JsonOut &add(const char *k, bool v)          { key(k); _s += v ? "true" : "false"; return *this; }
  JsonOut &add(const char *k, int v)           { key(k); _s += String(v); return *this; }
  JsonOut &add(const char *k, long v)          { key(k); _s += String(v); return *this; }
  JsonOut &add(const char *k, unsigned v)      { key(k); _s += String(v); return *this; }
  JsonOut &add(const char *k, unsigned long v) { key(k); _s += String(v); return *this; }
  JsonOut &add(const char *k, uint64_t v)      { key(k); _s += String((unsigned long long)v); return *this; }
  JsonOut &add(const char *k, double v, int decimals = 3) {
    key(k);
    if (isnan(v) || isinf(v)) _s += "null";
    else _s += String(v, decimals);
    return *this;
  }
  // 直接放入已是 JSON 的內容 (物件 / 陣列)
  JsonOut &raw(const char *k, const String &json) { key(k); _s += json; return *this; }

  String end() { _s += "}"; return _s; }

private:
  void key(const char *k) {
    if (!_first) _s += ',';
    _first = false;
    _s += '"';
    _s += k;
    _s += "\":";
  }
  String _s;
  bool _first = true;
};
