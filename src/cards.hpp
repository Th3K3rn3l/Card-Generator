#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace cards {

struct Brand {
    std::string_view name;
    std::vector<std::string_view> prefixes;
    int length;
    int cvv;
};

inline const std::vector<Brand>& brands() {
    static const std::vector<Brand> B = {
        {"visa",       {"4"},                          16, 3},
        {"mc",         {"51","52","53","54","55"},     16, 3},
        {"mastercard", {"51","52","53","54","55"},     16, 3},
        {"amex",       {"34","37"},                    15, 4},
        {"discover",   {"6011","65"},                  16, 3},
        {"jcb",        {"35"},                         16, 3},
    };
    return B;
}

inline const Brand* find_brand(std::string_view name) {
    for (const auto& b : brands()) if (b.name == name) return &b;
    return nullptr;
}

inline bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// Pick a brand whose IIN prefix matches the given BIN. Returns the brand
// with the longest matching prefix, or nullptr.
inline const Brand* detect_brand_from_bin(std::string_view bin) {
    const Brand* best = nullptr;
    size_t best_len = 0;
    for (const auto& b : brands()) {
        for (auto p : b.prefixes) {
            if (p.size() <= bin.size() &&
                bin.compare(0, p.size(), p) == 0 &&
                p.size() > best_len) {
                best = &b;
                best_len = p.size();
            }
        }
    }
    return best;
}

// xoshiro256** — small, fast, high quality.
struct Xoshiro {
    uint64_t s[4];

    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    void seed(uint64_t extra) {
        if (extra == 0) {
            std::random_device rd;
            uint64_t z = (static_cast<uint64_t>(rd()) << 32) ^ rd();
            z ^= static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            extra = z ? z : 0xa5a5a5a5a5a5a5a5ULL;
        }
        uint64_t z = extra;
        for (int i = 0; i < 4; i++) {
            z += 0x9e3779b97f4a7c15ULL;
            uint64_t t = z;
            t = (t ^ (t >> 30)) * 0xbf58476d1ce4e5b9ULL;
            t = (t ^ (t >> 27)) * 0x94d049bb133111ebULL;
            t = t ^ (t >> 31);
            s[i] = t ? t : 0xdeadbeefULL;
        }
    }

    void seed_random() { seed(0); }

    inline uint64_t next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1];
        s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
};

// Uniform 0..n-1 via Lemire's multiplication trick (bias negligible for our n).
inline uint32_t bounded(Xoshiro& r, uint32_t n) {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(static_cast<uint32_t>(r.next())) * n) >> 32);
}

inline bool luhn_check(std::string_view num) {
    int sum = 0;
    const size_t len = num.size();
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        int c = num[len - 1 - i] - '0';
        if (c < 0 || c > 9) return false;
        if (i & 1) { c += c; if (c > 9) c -= 9; }
        sum += c;
    }
    return sum % 10 == 0;
}

enum class OutputFormat { Plain, Json, Csv };

struct Options {
    const Brand* brand = nullptr;
    bool format = true;            // group digits by 4 with spaces (plain only)
    std::string separator = " ";   // between fields (plain / csv)
    int year_span = 6;             // default window for random year
    std::string custom_prefix;     // BIN override

    // Fixed values (0 / -1 / empty means "random").
    int fixed_month = 0;           // 1..12 or 0
    int fixed_year = -1;           // 0..99 or -1
    int year_min = -1;             // 2-digit lower bound, -1 = auto
    int year_max = -1;             // 2-digit upper bound, -1 = auto
    std::string fixed_cvv;         // empty = random

    // Length overrides.
    int length_override = 0;       // 0 = brand default
    int cvv_length_override = 0;   // 0 = brand default

    bool enforce_luhn = true;
    bool seed_set = false;
    uint64_t seed = 0;

    OutputFormat output = OutputFormat::Plain;
    bool header = false;           // emit a CSV-style header line first
};

inline int effective_length(const Options& opt) {
    return opt.length_override > 0 ? opt.length_override : opt.brand->length;
}

inline int effective_cvv_length(const Options& opt) {
    return opt.cvv_length_override > 0 ? opt.cvv_length_override : opt.brand->cvv;
}

constexpr int MAX_NUMBER_DIGITS = 24;
constexpr int MAX_CVV_DIGITS = 8;

struct RawFields {
    uint8_t number_digits[MAX_NUMBER_DIGITS];
    int number_len;
    int month;   // 1..12
    int year;    // 0..99
    uint8_t cvv_digits[MAX_CVV_DIGITS];
    int cvv_len;
};

inline void generate_fields(RawFields& f, Xoshiro& rng, const Options& opt,
                            const std::vector<std::vector<uint8_t>>& pref_d,
                            int year_base) {
    const int total = effective_length(opt);
    const int cvv_len = effective_cvv_length(opt);
    f.number_len = total;
    f.cvv_len = cvv_len;

    const size_t prefn = pref_d.size();
    const uint32_t pi = (prefn > 1) ? bounded(rng, static_cast<uint32_t>(prefn)) : 0;
    const auto& pa = pref_d[pi];
    const int plen = static_cast<int>(pa.size());

    int sum = 0;
    const int last_idx = opt.enforce_luhn ? total - 1 : total;
    for (int j = 0; j < last_idx; j++) {
        int d = (j < plen) ? pa[j] : static_cast<int>(bounded(rng, 10));
        f.number_digits[j] = static_cast<uint8_t>(d);
        if (opt.enforce_luhn) {
            const int pos = total - j;
            if ((pos & 1) == 0) {
                int dd = d + d;
                sum += (dd > 9) ? (dd - 9) : dd;
            } else {
                sum += d;
            }
        }
    }
    if (opt.enforce_luhn) {
        const int check = (10 - sum % 10) % 10;
        f.number_digits[total - 1] = static_cast<uint8_t>(check);
    }

    if (opt.fixed_month > 0) {
        f.month = opt.fixed_month;
    } else {
        f.month = static_cast<int>(bounded(rng, 12)) + 1;
    }

    if (opt.fixed_year >= 0) {
        f.year = opt.fixed_year;
    } else {
        int ymin = (opt.year_min >= 0) ? opt.year_min : (year_base + 1);
        int ymax = (opt.year_max >= 0) ? opt.year_max : (ymin + opt.year_span - 1);
        if (ymax < ymin) ymax = ymin;
        const uint32_t span = static_cast<uint32_t>(ymax - ymin + 1);
        int y = ymin + static_cast<int>(bounded(rng, span));
        y %= 100; if (y < 0) y += 100;
        f.year = y;
    }

    if (!opt.fixed_cvv.empty()) {
        const int n = std::min<int>(cvv_len, static_cast<int>(opt.fixed_cvv.size()));
        for (int k = 0; k < n; k++) {
            f.cvv_digits[k] = static_cast<uint8_t>(opt.fixed_cvv[k] - '0');
        }
        for (int k = n; k < cvv_len; k++) {
            f.cvv_digits[k] = static_cast<uint8_t>(bounded(rng, 10));
        }
    } else {
        for (int k = 0; k < cvv_len; k++) {
            f.cvv_digits[k] = static_cast<uint8_t>(bounded(rng, 10));
        }
    }
}

inline char* write_plain(char* w, const RawFields& f, const Options& opt) {
    const int total = f.number_len;
    const bool format = opt.format;
    const size_t sep_len = opt.separator.size();
    const char* sep = opt.separator.data();

    for (int j = 0; j < total; j++) {
        if (format && j > 0 && (j & 3) == 0) *w++ = ' ';
        *w++ = static_cast<char>('0' + f.number_digits[j]);
    }
    if (sep_len == 1) *w++ = sep[0];
    else { std::memcpy(w, sep, sep_len); w += sep_len; }

    *w++ = static_cast<char>('0' + f.month / 10);
    *w++ = static_cast<char>('0' + f.month % 10);
    *w++ = '/';
    *w++ = static_cast<char>('0' + f.year / 10);
    *w++ = static_cast<char>('0' + f.year % 10);

    if (sep_len == 1) *w++ = sep[0];
    else { std::memcpy(w, sep, sep_len); w += sep_len; }

    for (int k = 0; k < f.cvv_len; k++) {
        *w++ = static_cast<char>('0' + f.cvv_digits[k]);
    }
    *w++ = '\n';
    return w;
}

inline char* write_json(char* w, const RawFields& f, const Options& /*opt*/) {
    static constexpr char K1[] = "{\"number\":\"";
    static constexpr char K2[] = "\",\"expiry\":\"";
    static constexpr char K3[] = "\",\"cvv\":\"";
    static constexpr char K4[] = "\"}\n";
    std::memcpy(w, K1, sizeof(K1) - 1); w += sizeof(K1) - 1;
    for (int j = 0; j < f.number_len; j++) *w++ = static_cast<char>('0' + f.number_digits[j]);
    std::memcpy(w, K2, sizeof(K2) - 1); w += sizeof(K2) - 1;
    *w++ = static_cast<char>('0' + f.month / 10);
    *w++ = static_cast<char>('0' + f.month % 10);
    *w++ = '/';
    *w++ = static_cast<char>('0' + f.year / 10);
    *w++ = static_cast<char>('0' + f.year % 10);
    std::memcpy(w, K3, sizeof(K3) - 1); w += sizeof(K3) - 1;
    for (int k = 0; k < f.cvv_len; k++) *w++ = static_cast<char>('0' + f.cvv_digits[k]);
    std::memcpy(w, K4, sizeof(K4) - 1); w += sizeof(K4) - 1;
    return w;
}

inline int max_line_length(const Options& opt) {
    const int total = effective_length(opt);
    const int cvv = effective_cvv_length(opt);
    const int sep_len = static_cast<int>(opt.separator.size());
    if (opt.output == OutputFormat::Json) {
        return 11 + total + 13 + 5 + 10 + cvv + 3;
    }
    const int card_str = opt.format ? total + (total + 3) / 4 - 1 : total;
    return card_str + sep_len + 5 + sep_len + cvv + 1;
}

template <class Sink>
void generate(long long count, Options opt, Sink&& sink) {
    if (opt.output != OutputFormat::Plain) opt.format = false;

    std::vector<std::vector<uint8_t>> pref_d;
    if (!opt.custom_prefix.empty()) {
        std::vector<uint8_t> v;
        v.reserve(opt.custom_prefix.size());
        for (char c : opt.custom_prefix) v.push_back(static_cast<uint8_t>(c - '0'));
        pref_d.push_back(std::move(v));
    } else {
        pref_d.reserve(opt.brand->prefixes.size());
        for (auto p : opt.brand->prefixes) {
            std::vector<uint8_t> v;
            v.reserve(p.size());
            for (char c : p) v.push_back(static_cast<uint8_t>(c - '0'));
            pref_d.push_back(std::move(v));
        }
    }

    const std::time_t now = std::time(nullptr);
    const std::tm tm_now = *std::localtime(&now);
    const int year_base = (tm_now.tm_year + 1900) % 100;

    const int lline = max_line_length(opt);
    constexpr int BATCH_BYTES = 64 * 1024;
    int per_batch = BATCH_BYTES / lline;
    if (per_batch < 1) per_batch = 1;

    std::vector<char> buf(static_cast<size_t>(per_batch) * static_cast<size_t>(lline) + 64);

    Xoshiro rng;
    if (opt.seed_set) rng.seed(opt.seed ? opt.seed : 0xc0ffeeULL);
    else rng.seed_random();

    if (opt.header && opt.output != OutputFormat::Json) {
        std::string h;
        h += "number";
        h += opt.separator;
        h += "expiry";
        h += opt.separator;
        h += "cvv\n";
        sink(h.data(), h.size());
    }

    long long remaining = count;
    while (remaining > 0) {
        const int n = static_cast<int>(remaining < per_batch ? remaining : per_batch);
        char* w = buf.data();
        for (int i = 0; i < n; i++) {
            RawFields f;
            generate_fields(f, rng, opt, pref_d, year_base);
            if (opt.output == OutputFormat::Json) w = write_json(w, f, opt);
            else w = write_plain(w, f, opt);
        }
        sink(buf.data(), static_cast<size_t>(w - buf.data()));
        remaining -= n;
    }
}

} // namespace cards
