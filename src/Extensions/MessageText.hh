#pragma once

#include <string>

namespace teaserv::extensions {

// 200 UTF-8 bytes encode to at most 400 UTF-16 bytes. This leaves room for
// language markers, the text header, terminator, alignment, and command header
// under the legacy 0x200-byte text limit, even for the largest BB framing.
inline std::string plain_message_text(const std::string& text) {
  std::string result;
  for (unsigned char ch : text) {
    if ((ch >= 0x20 && ch != 0x7F) || ch == '\n') {
      result.push_back(ch);
    }
  }
  if (result.size() > 200) {
    size_t end = 200;
    while (end && (static_cast<unsigned char>(result[end]) & 0xC0) == 0x80) {
      --end;
    }
    result.resize(end);
  }
  return result;
}

} // namespace teaserv::extensions
