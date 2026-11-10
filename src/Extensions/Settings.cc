#include "Settings.hh"

#include <algorithm>
#include <stdexcept>

namespace teaserv::extensions {
namespace {

void validate_name_type(const std::string& name, const std::string& type) {
  if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      }))
    throw std::runtime_error("invalid extension setting name");
  if (type != "boolean" && type != "integer" && type != "string") {
    throw std::runtime_error("unsupported type for setting: " + name);
  }
}

void validate_value(const std::string& name, const SettingDefinition& definition, const SettingValue& value) {
  bool valid = (definition.type == "boolean" && std::holds_alternative<bool>(value)) ||
      (definition.type == "integer" && std::holds_alternative<int64_t>(value)) ||
      (definition.type == "string" && std::holds_alternative<std::string>(value));
  if (!valid)
    throw std::runtime_error("wrong type for setting: " + name);
  if (const auto* number = std::get_if<int64_t>(&value)) {
    if (*number < definition.min || *number > definition.max)
      throw std::runtime_error("out-of-range setting: " + name);
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    if (text->size() > definition.max_bytes || text->find('\0') != std::string::npos) {
      throw std::runtime_error("oversized or NUL-containing setting: " + name);
    }
  }
}

} // namespace

Settings resolve_settings(const SettingsSchema& schema, const std::map<std::string, SettingValue>& supplied) {
  if (schema.size() > 64 || supplied.size() > 64)
    throw std::runtime_error("at most 64 extension settings are allowed");
  for (const auto& [name, value] : supplied) {
    (void)value;
    validate_name_type(name, "string");
    if (!schema.count(name))
      throw std::runtime_error("undeclared extension setting: " + name);
  }
  Settings result;
  for (const auto& [name, definition] : schema) {
    validate_name_type(name, definition.type);
    if (definition.min > definition.max || definition.max_bytes < 1 || definition.max_bytes > 1024) {
      throw std::runtime_error("invalid bounds for setting: " + name);
    }
    // Invalid defaults reject the schema even when an override would hide them.
    if (definition.default_value)
      validate_value(name, definition, *definition.default_value);
    auto it = supplied.find(name);
    auto value = it == supplied.end() ? definition.default_value : std::optional<SettingValue>(it->second);
    if (!value && definition.required)
      throw std::runtime_error("missing required setting: " + name);
    if (value)
      validate_value(name, definition, *value);
    result.emplace(name, Setting{definition.type, std::move(value)});
  }
  validate_settings(result);
  return result;
}

void validate_settings(const Settings& settings) {
  if (settings.size() > 64)
    throw std::runtime_error("at most 64 extension settings are allowed");
  size_t bytes = 0;
  for (const auto& [name, setting] : settings) {
    validate_name_type(name, setting.type);
    bytes += name.size();
    if (setting.value) {
      SettingDefinition definition;
      definition.type = setting.type;
      validate_value(name, definition, *setting.value);
      const auto* text = std::get_if<std::string>(&*setting.value);
      bytes += text ? text->size() : 8;
    }
  }
  if (bytes > 16 * 1024)
    throw std::runtime_error("extension settings exceed 16 KiB");
}

} // namespace teaserv::extensions
