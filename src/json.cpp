#include "rockid/json.hpp"

#include <cctype>
#include <sstream>

namespace rockid {
namespace {

class Parser {
 public:
  explicit Parser(const std::string &text) : s_(text) {}

  Json parse() {
    Json value = parse_value();
    skip();
    if (i_ != s_.size()) throw JsonError("BAD_JSON");
    return value;
  }

 private:
  Json parse_value() {
    skip();
    if (i_ >= s_.size()) throw JsonError("BAD_JSON");
    char c = s_[i_];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
    throw JsonError("BAD_JSON");
  }

  Json parse_object() {
    expect('{');
    Depth depth(depth_);
    Json obj = Json::object();
    skip();
    if (consume('}')) return obj;
    while (true) {
      skip();
      Json key = parse_string();
      skip();
      expect(':');
      obj.o.emplace(key.s, parse_value());
      skip();
      if (consume('}')) return obj;
      expect(',');
    }
  }

  Json parse_array() {
    expect('[');
    Depth depth(depth_);
    Json arr = Json::array();
    skip();
    if (consume(']')) return arr;
    while (true) {
      arr.a.push_back(parse_value());
      skip();
      if (consume(']')) return arr;
      expect(',');
    }
  }

  Json parse_string() {
    expect('"');
    std::string out;
    while (i_ < s_.size()) {
      unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return Json::str(std::move(out));
      if (c < 0x20) throw JsonError("BAD_JSON");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (i_ >= s_.size()) throw JsonError("BAD_JSON");
      char e = s_[i_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          append_utf8(out, read_hex4());
          break;
        default:
          throw JsonError("BAD_JSON");
      }
    }
    throw JsonError("BAD_JSON");
  }

  Json parse_bool() {
    if (match("true")) return Json::boolean(true);
    if (match("false")) return Json::boolean(false);
    throw JsonError("BAD_JSON");
  }

  Json parse_null() {
    if (!match("null")) throw JsonError("BAD_JSON");
    return Json::nul();
  }

  Json parse_number() {
    size_t start = i_;
    if (consume('-')) {
      /* sign only */
    }
    if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_])))
      throw JsonError("BAD_JSON");
    if (s_[i_] == '0') {
      ++i_;
    } else {
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool fractional = false;
    if (i_ < s_.size() && s_[i_] == '.') {
      fractional = true;
      ++i_;
      if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_])))
        throw JsonError("BAD_JSON");
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      fractional = true;
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_])))
        throw JsonError("BAD_JSON");
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    std::string token = s_.substr(start, i_ - start);
    try {
      if (!fractional) return Json::integer(std::stoll(token));
      return Json::floating(std::stod(token));
    } catch (const std::exception &) {
      throw JsonError("BAD_JSON");
    }
  }

  int read_hex4() {
    int cp = 0;
    for (int k = 0; k < 4; ++k) {
      if (i_ >= s_.size()) throw JsonError("BAD_JSON");
      unsigned char h = static_cast<unsigned char>(s_[i_++]);
      cp <<= 4;
      if (h >= '0' && h <= '9') cp += h - '0';
      else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
      else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
      else throw JsonError("BAD_JSON");
    }
    return cp;
  }

  static void append_utf8(std::string &out, int cp) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) throw JsonError("BAD_JSON");
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

  void skip() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
  }

  void expect(char c) {
    if (i_ >= s_.size() || s_[i_] != c) throw JsonError("BAD_JSON");
    ++i_;
  }

  bool consume(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }

  bool match(const char *lit) {
    size_t n = 0;
    while (lit[n]) ++n;
    if (s_.compare(i_, n, lit) != 0) return false;
    i_ += n;
    return true;
  }

  struct Depth {
    int &d;
    explicit Depth(int &d) : d(d) {
      if (d >= 64) throw JsonError("BAD_JSON");
      ++d;
    }
    ~Depth() { --d; }
  };

  const std::string &s_;
  size_t i_ = 0;
  int depth_ = 0;
};

void escape_string(std::string &out, const std::string &value) {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          static const char *hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0x0F]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void dump_into(std::string &out, const Json &value) {
  switch (value.type) {
    case Json::Type::Null:
      out += "null";
      break;
    case Json::Type::Bool:
      out += value.b ? "true" : "false";
      break;
    case Json::Type::Int:
      out += std::to_string(value.i);
      break;
    case Json::Type::Float: {
      std::ostringstream os;
      os.imbue(std::locale::classic());
      os.precision(15);
      os << value.f;
      out += os.str();
      break;
    }
    case Json::Type::String:
      escape_string(out, value.s);
      break;
    case Json::Type::Array: {
      out.push_back('[');
      for (size_t n = 0; n < value.a.size(); ++n) {
        if (n) out.push_back(',');
        dump_into(out, value.a[n]);
      }
      out.push_back(']');
      break;
    }
    case Json::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto &kv : value.o) {
        if (!first) out.push_back(',');
        first = false;
        escape_string(out, kv.first);
        out.push_back(':');
        dump_into(out, kv.second);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

Json Json::nul() { return Json{}; }

Json Json::boolean(bool value) {
  Json j;
  j.type = Type::Bool;
  j.b = value;
  return j;
}

Json Json::integer(int64_t value) {
  Json j;
  j.type = Type::Int;
  j.i = value;
  return j;
}

Json Json::floating(double value) {
  Json j;
  j.type = Type::Float;
  j.f = value;
  return j;
}

Json Json::str(std::string value) {
  Json j;
  j.type = Type::String;
  j.s = std::move(value);
  return j;
}

Json Json::array() {
  Json j;
  j.type = Type::Array;
  return j;
}

Json Json::object() {
  Json j;
  j.type = Type::Object;
  return j;
}

const Json *Json::find(const std::string &key) const {
  if (type != Type::Object) return nullptr;
  auto it = o.find(key);
  if (it == o.end()) return nullptr;
  return &it->second;
}

Json parse_json(const std::string &text) { return Parser(text).parse(); }

std::string dump_json(const Json &value) {
  std::string out;
  dump_into(out, value);
  return out;
}

Json error_response(const Json &id, int code, const std::string &message) {
  Json err = Json::object();
  err.o["code"] = Json::integer(code);
  err.o["message"] = Json::str(message);
  Json resp = Json::object();
  resp.o["id"] = id;
  resp.o["ok"] = Json::boolean(false);
  resp.o["error"] = std::move(err);
  return resp;
}

std::string require_string(const Json &object, const char *key) {
  const Json *v = object.find(key);
  if (!v || !v->is_string()) throw ApiError(400, "BAD_PARAMS");
  return v->s;
}

int64_t require_int(const Json &object, const char *key) {
  const Json *v = object.find(key);
  if (!v || !v->is_int()) throw ApiError(400, "BAD_PARAMS");
  return v->i;
}

std::optional<std::string> optional_string(const Json &object, const char *key) {
  const Json *v = object.find(key);
  if (!v || v->is_null()) return std::nullopt;
  if (!v->is_string()) throw ApiError(400, "BAD_PARAMS");
  return v->s;
}

std::optional<int64_t> optional_int(const Json &object, const char *key) {
  const Json *v = object.find(key);
  if (!v || v->is_null()) return std::nullopt;
  if (!v->is_int()) throw ApiError(400, "BAD_PARAMS");
  return v->i;
}

std::optional<bool> optional_bool(const Json &object, const char *key) {
  const Json *v = object.find(key);
  if (!v || v->is_null()) return std::nullopt;
  if (!v->is_bool()) throw ApiError(400, "BAD_PARAMS");
  return v->b;
}

}
