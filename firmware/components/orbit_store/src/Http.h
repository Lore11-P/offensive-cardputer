#pragma once

#include <cstddef>
#include <string>

namespace orbit_store {

bool httpGet(const std::string& url, std::string& out, size_t maxBytes,
             std::string* error);

}  // namespace orbit_store
