#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace teaserv::extensions {

struct CatalogCommand {
  std::string package_id;
  std::string name;
  std::string permission;
  uint32_t cooldown_ms;
  std::string usage;
  std::string help;
};

// The adapter supplies only enabled extension commands, never built-ins.
// Permission filtering happens before constructing any Lua table.
void validate_command_catalog(const std::vector<CatalogCommand>& catalog);

} // namespace teaserv::extensions
