// In-memory stand-in for the ESP32 Preferences (NVS) API.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <stdint.h>
#include <string.h>

class Preferences {
 public:
  static std::map<std::string, std::vector<uint8_t>>& store() { static std::map<std::string, std::vector<uint8_t>> s; return s; }
  bool begin(const char* ns, bool readOnly = false) {
    ns_ = ns;
    if (readOnly) { for (auto& kv : store()) if (kv.first.rfind(ns_ + "/", 0) == 0) return true; return false; }
    return true;
  }
  void end() {}
  size_t putBytes(const char* k, const void* v, size_t n) { store()[ns_ + "/" + k].assign((const uint8_t*)v, (const uint8_t*)v + n); return n; }
  size_t getBytesLength(const char* k) { auto it = store().find(ns_ + "/" + k); return it == store().end() ? 0 : it->second.size(); }
  size_t getBytes(const char* k, void* out, size_t n) {
    auto it = store().find(ns_ + "/" + k);
    if (it == store().end()) return 0;
    size_t m = it->second.size() < n ? it->second.size() : n;
    memcpy(out, it->second.data(), m); return m;
  }
 private:
  std::string ns_;
};
