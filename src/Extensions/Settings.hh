#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace teaserv::extensions {

using SettingValue = std::variant<bool, int64_t, std::string>;
struct SettingDefinition {
  std::string type;
  bool required = false;
  std::optional<SettingValue> default_value = {};
  int64_t min = INT64_MIN;
  int64_t max = INT64_MAX;
  size_t max_bytes = 1024;
};
struct Setting {
  std::string type;
  std::optional<SettingValue> value;
};
using Settings = std::map<std::string, Setting>;
using SettingsSchema = std::map<std::string, SettingDefinition>;

// No Newserv/JSON/Lua dependency. Diagnostics identify names, never values.
Settings resolve_settings(const SettingsSchema& schema, const std::map<std::string, SettingValue>& supplied);
void validate_settings(const Settings& settings);

} // namespace teaserv::extensions
