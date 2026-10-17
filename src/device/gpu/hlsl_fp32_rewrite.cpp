#include "hlsl_fp32_rewrite.h"

#include <cctype>
#include <string_view>

namespace prism::device::gpu {

namespace {

bool IdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// double, double2..4, double2x2..4x4 -> the float spelling; empty if not a double type.
std::string FloatSpelling(std::string_view word)
{
    if (!word.starts_with("double")) return {};
    std::string_view rest = word.substr(6);
    auto dim = [](char c) { return c >= '1' && c <= '4'; };
    if (rest.empty() || (rest.size() == 1 && dim(rest[0])) ||
        (rest.size() == 3 && dim(rest[0]) && rest[1] == 'x' && dim(rest[2])))
        return "float" + std::string(rest);
    return {};
}

} // namespace

bool RewriteHlslToFp32(std::string const& code, std::string* result, std::string* error)
{
    std::string out;
    out.reserve(code.size());
    size_t i = 0;
    const size_t n = code.size();
    while (i < n) {
        const char c = code[i];
        // Comments and string literals are copied verbatim.
        if (c == '/' && i + 1 < n && code[i + 1] == '/') {
            const size_t end = code.find('\n', i);
            const size_t stop = end == std::string::npos ? n : end;
            out.append(code, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '*') {
            const size_t end = code.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? n : end + 2;
            out.append(code, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && code[j] != '"') j += code[j] == '\\' ? 2 : 1;
            j = std::min(n, j + 1);
            out.append(code, i, j - i);
            i = j;
            continue;
        }
        if (IdentifierStart(c)) {
            size_t j = i + 1;
            while (j < n && IdentifierChar(code[j])) ++j;
            const std::string_view word(code.data() + i, j - i);
            const std::string replacement = FloatSpelling(word);
            out.append(replacement.empty() ? std::string(word) : replacement);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
            // A numeric literal, possibly with an exponent and suffixes.
            size_t j = i;
            bool floating = false;
            const bool hex = c == '0' && j + 1 < n && (code[j + 1] == 'x' || code[j + 1] == 'X');
            if (hex) j += 2;
            while (j < n) {
                const char d = code[j];
                if (std::isxdigit(static_cast<unsigned char>(d)) && (hex || std::isdigit(static_cast<unsigned char>(d)) ||
                                                                     d == 'e' || d == 'E')) {
                    if (!hex && (d == 'e' || d == 'E')) {
                        floating = true;
                        if (j + 1 < n && (code[j + 1] == '+' || code[j + 1] == '-')) ++j;
                    }
                    ++j;
                } else if (d == '.') {
                    floating = true;
                    ++j;
                } else {
                    break;
                }
            }
            out.append(code, i, j - i);
            // Suffixes: double literals (l/L) become float literals (f).
            while (j < n && IdentifierChar(code[j])) {
                const char s = code[j];
                out.push_back(floating && (s == 'l' || s == 'L') ? 'f' : s);
                ++j;
            }
            i = j;
            continue;
        }
        out.push_back(c);
        ++i;
    }

    // Reject any double type left (in code, not identifiers or comments).
    for (size_t k = 0; k < out.size();) {
        if (out.compare(k, 2, "//") == 0) {
            const size_t end = out.find('\n', k);
            k = end == std::string::npos ? out.size() : end;
            continue;
        }
        if (IdentifierStart(out[k])) {
            size_t j = k + 1;
            while (j < out.size() && IdentifierChar(out[j])) ++j;
            if (std::string_view(out.data() + k, j - k) == "double") {
                if (error) *error = "generated HLSL still uses the double type";
                return false;
            }
            k = j;
            continue;
        }
        ++k;
    }
    *result = std::move(out);
    return true;
}

} // namespace prism::device::gpu
