#pragma once

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

// xoshiro256** — small, fast, high quality.
struct Xoshiro {
    uint64_t s[4];

    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    void seed(uint64_t extra = 0) {
        std::random_device rd;
        uint64_t z = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        z ^= static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        z ^= extra;
        for (int i = 0; i < 4; i++) {
            z += 0x9e3779b97f4a7c15ULL;
            uint64_t t = z;
            t = (t ^ (t >> 30)) * 0xbf58476d1ce4e5b9ULL;
            t = (t ^ (t >> 27)) * 0x94d049bb133111ebULL;
            t = t ^ (t >> 31);
            s[i] = t ? t : 0xdeadbeefULL;
        }
    }

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

// Uniform 0..n-1 from a 32-bit slice via Lemire's multiplication trick.
// Bias is negligible for the small n we use here (10, 12, year span).
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

struct Options {
    const Brand* brand = nullptr;
    bool format = true;        // group digits by 4 with spaces
    std::string separator = " ";
    int year_span = 6;
};

// Writes one card record into `w`, returns the new write pointer.
inline char* write_card(char* w, Xoshiro& rng, const Options& opt,
                        const std::vector<std::array<uint8_t, 4>>& pref_d,
                        const std::vector<int>& pref_len,
                        int year_base) {
    const int total = opt.brand->length;
    const int cvv = opt.brand->cvv;
    const bool format = opt.format;
    const size_t sep_len = opt.separator.size();
    const char* sep = opt.separator.data();

    const uint32_t pi = bounded(rng, static_cast<uint32_t>(pref_d.size()));
    const auto& pa = pref_d[pi];
    const int plen = pref_len[pi];

    int sum = 0;
    for (int j = 0; j < total - 1; j++) {
        int d = (j < plen) ? pa[j] : static_cast<int>(bounded(rng, 10));
        const int pos = total - j;
        if ((pos & 1) == 0) {
            int dd = d + d;
            sum += (dd > 9) ? (dd - 9) : dd;
        } else {
            sum += d;
        }
        if (format && j > 0 && (j & 3) == 0) *w++ = ' ';
        *w++ = static_cast<char>('0' + d);
    }
    const int check = (10 - sum % 10) % 10;
    {
        const int j = total - 1;
        if (format && j > 0 && (j & 3) == 0) *w++ = ' ';
        *w++ = static_cast<char>('0' + check);
    }

    if (sep_len == 1) *w++ = sep[0];
    else { std::memcpy(w, sep, sep_len); w += sep_len; }

    const int month = static_cast<int>(bounded(rng, 12)) + 1;
    *w++ = static_cast<char>('0' + month / 10);
    *w++ = static_cast<char>('0' + month % 10);
    *w++ = '/';
    const int yr = (year_base + 1 + static_cast<int>(bounded(rng, opt.year_span))) % 100;
    *w++ = static_cast<char>('0' + yr / 10);
    *w++ = static_cast<char>('0' + yr % 10);

    if (sep_len == 1) *w++ = sep[0];
    else { std::memcpy(w, sep, sep_len); w += sep_len; }

    for (int k = 0; k < cvv; k++) {
        *w++ = static_cast<char>('0' + static_cast<int>(bounded(rng, 10)));
    }
    *w++ = '\n';
    return w;
}

inline int line_length(const Options& opt) {
    const int total = opt.brand->length;
    const int card_str = opt.format ? total + (total + 3) / 4 - 1 : total;
    const int sep_len = static_cast<int>(opt.separator.size());
    return card_str + sep_len + 5 + sep_len + opt.brand->cvv + 1;
}

// Generates `count` records, writing into `sink(ptr, len)`.
template <class Sink>
void generate(long long count, const Options& opt, Sink&& sink) {
    std::vector<std::array<uint8_t, 4>> pref_d;
    std::vector<int> pref_len;
    pref_d.reserve(opt.brand->prefixes.size());
    pref_len.reserve(opt.brand->prefixes.size());
    for (auto p : opt.brand->prefixes) {
        std::array<uint8_t, 4> a{};
        for (size_t i = 0; i < p.size() && i < 4; i++) a[i] = static_cast<uint8_t>(p[i] - '0');
        pref_d.push_back(a);
        pref_len.push_back(static_cast<int>(p.size()));
    }

    const std::time_t now = std::time(nullptr);
    const std::tm tm_now = *std::localtime(&now);
    const int year_base = (tm_now.tm_year + 1900) % 100;

    const int lline = line_length(opt);
    constexpr int BATCH_BYTES = 64 * 1024;
    int per_batch = BATCH_BYTES / lline;
    if (per_batch < 1) per_batch = 1;

    std::vector<char> buf(static_cast<size_t>(per_batch) * static_cast<size_t>(lline) + 64);

    Xoshiro rng;
    rng.seed();

    long long remaining = count;
    while (remaining > 0) {
        const int n = static_cast<int>(remaining < per_batch ? remaining : per_batch);
        char* w = buf.data();
        for (int i = 0; i < n; i++) {
            w = write_card(w, rng, opt, pref_d, pref_len, year_base);
        }
        sink(buf.data(), static_cast<size_t>(w - buf.data()));
        remaining -= n;
    }
}

} // namespace cards
