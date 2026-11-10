#include "CommandCatalog.hh"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace teaserv::extensions {

void validate_command_catalog(const std::vector<CatalogCommand>& catalog) {
  if (catalog.size() > 16 * 32) throw std::runtime_error("invalid command catalog size");
  auto name = [](const std::string& value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
  };
  auto text = [](const std::string& value, size_t limit) {
    return value.size() <= limit && std::none_of(value.begin(), value.end(), [](unsigned char c) {
      return c < 0x20 || c == 0x7F;
    });
  };
  static const std::set<std::string> permissions{"everyone", "announce", "debug", "moderator", "root", "change-event"};
  std::set<std::string> names;
  std::map<std::string, size_t> counts;
  for (const auto& command : catalog) {
    if (!name(command.name) || !name(command.package_id) || !names.emplace(command.name).second ||
        !permissions.count(command.permission) || command.cooldown_ms > 3600000 ||
        !text(command.usage, 512) || command.usage.empty() || !text(command.help, 160) ||
        ++counts[command.package_id] > 32 || counts.size() > 16) {
      throw std::runtime_error("invalid command catalog metadata");
    }
  }
}

} // namespace teaserv::extensions
