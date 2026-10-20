#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rockid {

struct Json {
  enum class Type { Null, Bool, Int, Float, String, Array, Object };

  Type type = Type::Null;
  bool b = false;
  int64_t i = 0;
  double f = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;

  static Json nul();
  static Json boolean(bool value);
  static Json integer(int64_t value);
  static Json floating(double value);
  static Json str(std::string value);
  static Json array();
  static Json object();

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_int() const { return type == Type::Int; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }

  const Json *find(const std::string &key) const;
};

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string &message) : std::runtime_error(message) {}
};

class ApiError : public std::runtime_error {
 public:
  int code;
  ApiError(int code, std::string message)
      : std::runtime_error(std::move(message)), code(code) {}
};

Json parse_json(const std::string &text);
std::string dump_json(const Json &value);

Json error_response(const Json &id, int code, const std::string &message);

std::string require_string(const Json &object, const char *key);
int64_t require_int(const Json &object, const char *key);
std::optional<std::string> optional_string(const Json &object, const char *key);
std::optional<int64_t> optional_int(const Json &object, const char *key);

}
