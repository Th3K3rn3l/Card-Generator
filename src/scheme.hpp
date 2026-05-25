#pragma once

// Card scheme rules table (ISO/IEC 7812, plus public scheme docs).
//
// This is intentionally separate from the small `Brand` table the *generator*
// uses (cards.hpp). The generator only needs one canonical length/CVV per
// brand to emit cards; the *validator* needs the full universe of accepted
// PAN lengths, prefix ranges and CVV lengths so it can correctly answer
// "could this look like a real card from scheme X?".
//
// Sources:
//   * Visa     — Visa Core Rules and Visa Product and Service Rules.
//   * MC       — Mastercard Account Range table; the 2221-2720 range was
//                added to mainstream MC in 2017.
//   * Amex     — American Express GTNS docs: 34/37, 15-digit PANs, 4-digit
//                CID.
//   * Diners   — Diners Club International: 300-305, 3095, 36, 38, 39.
//   * Discover — Discover IIN spec: 6011, 622126-622925 (UnionPay co-brand
//                handoff), 644-649, 65.
//   * JCB      — JCB IIN: 3528-3589.
//   * Maestro  — 50, 56-69, 12-19 digit PANs.
//   * UnionPay — 62 and 81 (newer 81 series).
//   * Mir      — Bank of Russia / NSPK MIR: 2200-2204.
//   * RuPay    — NPCI RuPay: 60, 6521-6522, 81, 82, 508.
//   * Verve    — Interswitch Verve: 506099-506198, 650002-650027.
//   * Hipercard, Elo, Dankort — local schemes, common in BIN datasets.
//
// Numeric prefix ranges are stored as [low, high] inclusive at a fixed digit
// length, so range checks reduce to two integer comparisons. When several
// schemes' ranges overlap (e.g. Maestro and RuPay both claim 60xxxx), the
// detector picks the longest matching prefix; ties go to the order in which
// schemes appear in `schemes()` (Visa/MC/Amex/Discover/JCB are listed first
// because the BIN database we ship is heavily biased toward them).

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cards {

struct PrefixRange {
    uint64_t low;
    uint64_t high;
    uint8_t  len;   // number of digits in `low`/`high` (1..6)
};

struct Scheme {
    std::string_view name;          // "visa", "mastercard", ...
    std::string_view display;       // "Visa", "Mastercard", ...
    std::vector<PrefixRange> ranges;
    std::vector<uint8_t>     lengths;       // accepted PAN lengths, ascending
    std::vector<uint8_t>     cvv_lengths;   // accepted CVV/CVC lengths
    bool                     luhn_required = true;

    bool accepts_length(int n) const noexcept {
        for (auto L : lengths) if (static_cast<int>(L) == n) return true;
        return false;
    }
    bool accepts_cvv_length(int n) const noexcept {
        for (auto L : cvv_lengths) if (static_cast<int>(L) == n) return true;
        return false;
    }
    int min_length() const noexcept { return lengths.empty() ? 0 : lengths.front(); }
    int max_length() const noexcept { return lengths.empty() ? 0 : lengths.back();  }
};

// Take the first `n` digits of `digits` as a uint64. Caller guarantees the
// string contains only ASCII digits.
inline uint64_t prefix_value(std::string_view digits, uint8_t n) noexcept {
    uint64_t v = 0;
    const size_t lim = std::min<size_t>(n, digits.size());
    for (size_t i = 0; i < lim; ++i) v = v * 10u + static_cast<uint64_t>(digits[i] - '0');
    return v;
}

// Returns the matched prefix length, or 0 if none of the scheme's ranges fit.
// `narrow_out`, if non-null, is set to the inverse "specificity" — the size
// of the matching range (smaller = more specific).
inline uint8_t scheme_match_len(const Scheme& s, std::string_view digits,
                                uint64_t* narrow_out = nullptr) noexcept {
    uint8_t best = 0;
    uint64_t best_narrow = UINT64_MAX;
    for (const auto& r : s.ranges) {
        if (digits.size() < r.len) continue;
        const uint64_t v = prefix_value(digits, r.len);
        if (v < r.low || v > r.high) continue;
        const uint64_t narrow = r.high - r.low + 1;
        if (r.len > best || (r.len == best && narrow < best_narrow)) {
            best = r.len;
            best_narrow = narrow;
        }
    }
    if (narrow_out) *narrow_out = (best == 0) ? UINT64_MAX : best_narrow;
    return best;
}

inline const std::vector<Scheme>& schemes() {
    static const std::vector<Scheme> S = []{
        std::vector<Scheme> s;
        s.reserve(16);

        // ---- Visa ---------------------------------------------------------
        s.push_back(Scheme{
            "visa", "Visa",
            { {4, 4, 1} },
            {13, 16, 19},
            {3},
            true
        });

        // ---- Mastercard ---------------------------------------------------
        s.push_back(Scheme{
            "mastercard", "Mastercard",
            {
                {51, 55, 2},          // legacy 51-55
                {2221, 2720, 4},      // 2017+ expansion
            },
            {16},
            {3},
            true
        });

        // ---- American Express ---------------------------------------------
        s.push_back(Scheme{
            "amex", "American Express",
            { {34, 34, 2}, {37, 37, 2} },
            {15},
            {4},
            true
        });

        // ---- Discover -----------------------------------------------------
        s.push_back(Scheme{
            "discover", "Discover",
            {
                {6011, 6011, 4},
                {622126, 622925, 6},  // UnionPay co-brand handoff
                {644, 649, 3},
                {65, 65, 2},
            },
            {16, 19},
            {3},
            true
        });

        // ---- JCB ----------------------------------------------------------
        s.push_back(Scheme{
            "jcb", "JCB",
            { {3528, 3589, 4} },
            {16, 19},
            {3},
            true
        });

        // ---- Diners Club International ------------------------------------
        s.push_back(Scheme{
            "diners", "Diners Club",
            {
                {300, 305, 3},
                {3095, 3095, 4},
                {36, 36, 2},
                {38, 39, 2},
            },
            {14, 16, 19},
            {3},
            true
        });

        // ---- Maestro ------------------------------------------------------
        // 50, 56-69, except those overlapping more specific schemes above.
        s.push_back(Scheme{
            "maestro", "Maestro",
            {
                {50, 50, 2},
                {56, 69, 2},
            },
            {12, 13, 14, 15, 16, 17, 18, 19},
            {3},
            true
        });

        // ---- China UnionPay -----------------------------------------------
        s.push_back(Scheme{
            "unionpay", "UnionPay",
            { {62, 62, 2}, {81, 81, 2} },
            {16, 17, 18, 19},
            {3},
            true
        });

        // ---- Mir (NSPK, Russia) -------------------------------------------
        s.push_back(Scheme{
            "mir", "Mir",
            { {2200, 2204, 4} },
            {16, 17, 18, 19},
            {3},
            true
        });

        // ---- RuPay (NPCI, India) ------------------------------------------
        s.push_back(Scheme{
            "rupay", "RuPay",
            {
                {6521, 6522, 4},
                {508, 508, 3},
                {60, 60, 2},
                {81, 82, 2},
            },
            {16},
            {3},
            true
        });

        // ---- Verve --------------------------------------------------------
        s.push_back(Scheme{
            "verve", "Verve",
            {
                {506099, 506198, 6},
                {650002, 650027, 6},
            },
            {16, 19},
            {3},
            true
        });

        // ---- Hipercard ----------------------------------------------------
        s.push_back(Scheme{
            "hipercard", "Hipercard",
            { {606282, 606282, 6} },
            {14, 15, 16, 17, 18, 19},
            {3},
            true
        });

        // ---- Dankort ------------------------------------------------------
        s.push_back(Scheme{
            "dankort", "Dankort",
            { {5019, 5019, 4} },
            {16},
            {3},
            true
        });

        // ---- InterPayment -------------------------------------------------
        s.push_back(Scheme{
            "interpayment", "InterPayment",
            { {636, 636, 3} },
            {16, 17, 18, 19},
            {3},
            true
        });

        // ---- InstaPayment -------------------------------------------------
        s.push_back(Scheme{
            "instapayment", "InstaPayment",
            { {637, 639, 3} },
            {16},
            {3},
            true
        });

        return s;
    }();
    return S;
}

inline const Scheme* find_scheme(std::string_view name) {
    for (const auto& s : schemes()) if (s.name == name) return &s;
    return nullptr;
}

// Detect the most specific scheme for a given digit string. Selection rules:
//   1. Longest matching prefix length wins.
//   2. On ties, the scheme whose matching range is *narrower* (covers fewer
//      candidate values) wins — i.e. UnionPay's "62" beats Maestro's "56-69"
//      because the former is a single value and the latter spans 14.
//   3. On full ties, the scheme listed first in `schemes()` wins.
inline const Scheme* detect_scheme(std::string_view digits,
                                   uint8_t* match_len_out = nullptr) noexcept {
    const Scheme* best = nullptr;
    uint8_t   best_len    = 0;
    uint64_t  best_narrow = UINT64_MAX;
    for (const auto& s : schemes()) {
        uint64_t narrow = UINT64_MAX;
        const uint8_t m = scheme_match_len(s, digits, &narrow);
        if (m == 0) continue;
        if (m > best_len ||
            (m == best_len && narrow < best_narrow)) {
            best        = &s;
            best_len    = m;
            best_narrow = narrow;
        }
    }
    if (match_len_out) *match_len_out = best_len;
    return best;
}

// Find every scheme that claims `digits` (used for ambiguity diagnostics —
// e.g. "60xxxx" matches both Maestro and RuPay).
inline std::vector<const Scheme*> all_matching_schemes(std::string_view digits) {
    std::vector<const Scheme*> out;
    uint8_t best_len = 0;
    for (const auto& s : schemes()) {
        const uint8_t m = scheme_match_len(s, digits);
        if (m > best_len) best_len = m;
    }
    if (best_len == 0) return out;
    for (const auto& s : schemes()) {
        if (scheme_match_len(s, digits) == best_len) out.push_back(&s);
    }
    return out;
}

} // namespace cards
