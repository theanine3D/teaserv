#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace teaserv::extensions {

struct CommandArgument {
  enum class Type { WORD,
    TEXT,
    INTEGER,
    CLIENT_ID };
  std::string name;
  Type type = Type::WORD;
  bool optional = false;
  size_t max_bytes = 256;
  int64_t minimum = INT32_MIN;
  int64_t maximum = INT32_MAX;
};

using CommandArgumentValue = std::variant<std::string, int64_t>;
using ParsedCommandArguments = std::vector<std::pair<std::string, CommandArgumentValue>>;

// Absent schema preserves raw-string behavior; an empty schema means no arguments.
class CommandArguments {
public:
  explicit CommandArguments(std::vector<CommandArgument> fields);
  std::optional<ParsedCommandArguments> parse(const std::string& input) const;
  std::string usage(const std::string& command) const;

private:
  std::vector<CommandArgument> fields;
};

} // namespace teaserv::extensions
