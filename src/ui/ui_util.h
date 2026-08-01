#pragma once

#include <string>
#include <algorithm>

/* ── Case-insensitive substring match (used by search filters) ── */
inline bool str_icontains(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}
