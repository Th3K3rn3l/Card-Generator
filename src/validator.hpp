#pragma once

// Multi-issue, scheme-aware payment-card validator.
//
// What this module is, and what it isn't.
//
// IS: a structural validator that takes a user-provided card record
//     (number, optional expiry, optional CVV, optional metadata) and answers
//     every question that can be answered offline:
//       * Is the input a sequence of digits after Unicode normalization?
//       * Does the digit length match the scheme's accepted PAN lengths?
//       * Is the BIN inside any known scheme range?
//       * Does Luhn check out?
//       * Is the expiry parseable, in the future, and within plausible bounds?
//       * Is the CVV the right length for the scheme?
//       * Does it look like a known *test* card from Stripe/Adyen/Braintree?
//       * Does it look like a low-entropy "11111111..." sentinel?
//       * If a BIN database is supplied, does the BIN actually appear there,
//         and does scheme/type/country agree with caller-supplied claims?
//
// IS NOT: a CVK / iCVV / EMV cryptographic check (impossible offline), nor an
//     "is this card live at the issuer" check (impossible without a network
//     authorization request). The validator clearly labels what it can and
//     cannot answer.
//
// The output is a structured `Report` with per-issue Severity (Error /
// Warning / Info), so callers can route results into CI gates, JSON logs or
// human-readable diagnostics.

#include "bin_db.hpp"
#include "cards.hpp"
#include "scheme.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace cards {

// ---------------------------------------------------------------------------
// Severity / issue codes
// ---------------------------------------------------------------------------

enum class Severity : uint8_t { Info = 0, Warning = 1, Error = 2 };

inline const char* to_string(Severity s) {
    switch (s) {
        case Severity::Info:    return "info";
        case Severity::Warning: return "warning";
        case Severity::Error:   return "error";
    }
    return "unknown";
}

// Stable, machine-readable issue codes. We keep the numeric values explicit so
// downstream tooling can pin to a specific code without re-reading the source.
enum class IssueCode : uint16_t {
    // ---- Structural (errors) ----------------------------------------------
    Empty                  = 1001,
    NonDigit               = 1002,
    NonAsciiInputAccepted  = 1003,  // info: we normalized away non-ASCII
    BadLength              = 1004,
    BadLuhn                = 1005,
    UnknownScheme          = 1006,
    SchemeMismatch         = 1007,
    PrefixOutOfRange       = 1008,
    BadMonth               = 1009,
    BadYear                = 1010,
    Expired                = 1011,
    ImplausibleFutureExpiry= 1012,
    BadCvvLength           = 1013,
    BadCvvDigits           = 1014,

    // ---- Quality / suspicion (warnings) -----------------------------------
    KnownTestCard          = 2001,  // matches a published gateway test PAN
    LowEntropy             = 2002,  // long runs / repeats / sequential digits
    SequentialDigits       = 2003,
    AllSameDigit           = 2004,
    AmbiguousScheme        = 2005,  // multiple schemes claim this prefix
    DbBinUnknown           = 2006,  // BIN DB loaded but BIN not present
    DbSchemeDisagrees      = 2007,
    DbCountryDisagrees     = 2008,
    DbTypeDisagrees        = 2009,

    // ---- Notes / informational --------------------------------------------
    NormalizedSeparators   = 3001,  // info: stripped spaces/dashes/tabs
    CvkNotChecked          = 3002,  // explicit reminder we can't verify CVV
};

inline const char* to_string(IssueCode c) {
    switch (c) {
        case IssueCode::Empty:                  return "empty";
        case IssueCode::NonDigit:               return "non_digit";
        case IssueCode::NonAsciiInputAccepted:  return "non_ascii_accepted";
        case IssueCode::BadLength:              return "bad_length";
        case IssueCode::BadLuhn:                return "bad_luhn";
        case IssueCode::UnknownScheme:          return "unknown_scheme";
        case IssueCode::SchemeMismatch:         return "scheme_mismatch";
        case IssueCode::PrefixOutOfRange:       return "prefix_out_of_range";
        case IssueCode::BadMonth:               return "bad_month";
        case IssueCode::BadYear:                return "bad_year";
        case IssueCode::Expired:                return "expired";
        case IssueCode::ImplausibleFutureExpiry:return "implausible_future_expiry";
        case IssueCode::BadCvvLength:           return "bad_cvv_length";
        case IssueCode::BadCvvDigits:           return "bad_cvv_digits";
        case IssueCode::KnownTestCard:          return "known_test_card";
        case IssueCode::LowEntropy:             return "low_entropy";
        case IssueCode::SequentialDigits:       return "sequential_digits";
        case IssueCode::AllSameDigit:           return "all_same_digit";
        case IssueCode::AmbiguousScheme:        return "ambiguous_scheme";
        case IssueCode::DbBinUnknown:           return "db_bin_unknown";
        case IssueCode::DbSchemeDisagrees:      return "db_scheme_disagrees";
        case IssueCode::DbCountryDisagrees:     return "db_country_disagrees";
        case IssueCode::DbTypeDisagrees:        return "db_type_disagrees";
        case IssueCode::NormalizedSeparators:   return "normalized_separators";
        case IssueCode::CvkNotChecked:          return "cvk_not_checked";
    }
    return "unknown_code";
}

struct Issue {
    Severity   severity;
    IssueCode  code;
    std::string message;        // human-readable detail
};

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

struct NormalizeReport {
    std::string digits;
    bool ok = true;             // false if any character could not be mapped
    bool stripped_separators = false;  // true if we removed spaces/dashes/etc.
    bool mapped_unicode = false;       // true if we mapped a non-ASCII digit
};

namespace detail {

// Decode one UTF-8 codepoint starting at `p` (with `end` as the buffer end).
// Returns the codepoint and advances `p`. On invalid bytes it returns U+FFFD
// and consumes one byte, so the loop always makes progress.
inline uint32_t utf8_next(const char*& p, const char* end) {
    if (p >= end) return 0;
    auto c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    int extra;
    uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { return 0xFFFD; }
    for (int i = 0; i < extra; ++i) {
        if (p >= end) return 0xFFFD;
        auto cc = static_cast<unsigned char>(*p++);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

// Map common non-ASCII digit codepoints to their ASCII counterparts.
// Returns -1 if the codepoint is not a digit we recognize.
inline int decimal_value(uint32_t cp) {
    // Fast path for ASCII.
    if (cp <= 0x7F) {
        return (cp >= '0' && cp <= '9') ? static_cast<int>(cp - '0') : -1;
    }
    // Curated table of ranges where U+xxxx + (digit) → ASCII digit. These are
    // the General_Category=Nd ranges that show up most often in user input
    // (full-width forms, Arabic-Indic, Devanagari, Bengali, etc). We don't
    // try to cover every Nd block in Unicode; we cover the ones that real
    // payment forms and IME paste behaviors actually produce.
    struct R { uint32_t base; };
    static constexpr R ranges[] = {
        {0x0660}, // Arabic-Indic
        {0x06F0}, // Extended Arabic-Indic
        {0x07C0}, // NKo
        {0x0966}, // Devanagari
        {0x09E6}, // Bengali
        {0x0A66}, // Gurmukhi
        {0x0AE6}, // Gujarati
        {0x0B66}, // Oriya
        {0x0BE6}, // Tamil
        {0x0C66}, // Telugu
        {0x0CE6}, // Kannada
        {0x0D66}, // Malayalam
        {0x0DE6}, // Sinhala Lith
        {0x0E50}, // Thai
        {0x0ED0}, // Lao
        {0x0F20}, // Tibetan
        {0x1040}, // Myanmar
        {0x1090}, // Myanmar Shan
        {0x17E0}, // Khmer
        {0x1810}, // Mongolian
        {0xFF10}, // Full-width 0..9
    };
    for (auto r : ranges) {
        if (cp >= r.base && cp <= r.base + 9) return static_cast<int>(cp - r.base);
    }
    return -1;
}

// Codepoints we silently treat as visual separators (in addition to the ASCII
// space/dash/tab/underscore already handled). Includes NBSP, narrow NBSP,
// figure space, BOM and zero-width spaces, all of which appear in real
// pasted card numbers from web forms / spreadsheets.
inline bool is_separator_codepoint(uint32_t cp) {
    switch (cp) {
        case ' ':
        case '\t':
        case '-':
        case '_':
        case 0x00A0: // NBSP
        case 0x2007: // FIGURE SPACE
        case 0x2009: // THIN SPACE
        case 0x200B: // ZERO WIDTH SPACE
        case 0x200C: // ZERO WIDTH NON-JOINER
        case 0x200D: // ZERO WIDTH JOINER
        case 0x2010: // HYPHEN
        case 0x2011: // NON-BREAKING HYPHEN
        case 0x2012: // FIGURE DASH
        case 0x2013: // EN DASH
        case 0x2014: // EM DASH
        case 0x202F: // NARROW NO-BREAK SPACE
        case 0xFEFF: // BOM / ZWNBSP
            return true;
        default:
            return false;
    }
}

} // namespace detail

// Normalize a card-number string written by humans into pure ASCII digits.
// We accept ASCII separators, common Unicode whitespace/dashes, and we map
// the major non-ASCII digit blocks (full-width 0..9, Arabic-Indic, etc.) to
// their ASCII counterparts. Anything else fails.
inline NormalizeReport normalize_number_full(std::string_view s) {
    NormalizeReport r;
    r.digits.reserve(s.size());
    const char* p   = s.data();
    const char* end = p + s.size();
    while (p < end) {
        const char* before = p;
        uint32_t cp = detail::utf8_next(p, end);
        if (cp == 0) break;
        const int dv = detail::decimal_value(cp);
        if (dv >= 0) {
            r.digits.push_back(static_cast<char>('0' + dv));
            if (cp > 0x7F) r.mapped_unicode = true;
            continue;
        }
        if (detail::is_separator_codepoint(cp)) {
            // ASCII space/dash/tab/underscore aren't visible "stripping" to
            // the user, but Unicode ones probably are — in either case mark
            // the report so the validator can emit an Info note.
            r.stripped_separators = true;
            continue;
        }
        (void)before;
        r.ok = false;
        r.digits.clear();
        return r;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Anti-pattern detectors
// ---------------------------------------------------------------------------

// Run-length & uniqueness statistics for a digit string.
struct DigitStats {
    int  longest_run = 0;       // longest run of the same digit
    int  longest_seq = 0;       // longest monotonic +1/-1 run (e.g. 12345)
    int  unique_digits = 0;     // count of distinct digit values
    bool all_same = false;
};

inline DigitStats analyze_digits(std::string_view digits) {
    DigitStats s;
    if (digits.empty()) return s;
    int run = 1, seq = 1;
    s.longest_run = 1;
    s.longest_seq = 1;
    bool seen[10] = {};
    seen[digits[0] - '0'] = true;
    for (size_t i = 1; i < digits.size(); ++i) {
        const int d  = digits[i]     - '0';
        const int pd = digits[i - 1] - '0';
        seen[d] = true;
        run = (d == pd) ? run + 1 : 1;
        const int delta = d - pd;
        if (delta == 1 || delta == -1) seq += 1;
        else                            seq  = 1;
        if (run > s.longest_run) s.longest_run = run;
        if (seq > s.longest_seq) s.longest_seq = seq;
    }
    for (bool b : seen) if (b) s.unique_digits++;
    s.all_same = (s.longest_run == static_cast<int>(digits.size()));
    return s;
}

// ---------------------------------------------------------------------------
// Known test PANs (Stripe / Adyen / Braintree / PayPal / Square)
// ---------------------------------------------------------------------------
//
// These are PANs published by major payment processors specifically so that
// developers can test integrations without ever hitting a real card. Every
// one of them is publicly documented and intentionally not associated with
// any real account.

inline const std::vector<std::pair<std::string_view, std::string_view>>& known_test_cards() {
    static const std::vector<std::pair<std::string_view, std::string_view>> T = {
        // Stripe: https://stripe.com/docs/testing
        {"4242424242424242", "Stripe / Visa success"},
        {"4000056655665556", "Stripe / Visa debit"},
        {"5555555555554444", "Stripe / Mastercard"},
        {"2223003122003222", "Stripe / Mastercard 2-series"},
        {"5200828282828210", "Stripe / Mastercard debit"},
        {"5105105105105100", "Stripe / Mastercard prepaid"},
        {"378282246310005",  "Stripe / Amex"},
        {"371449635398431",  "Stripe / Amex"},
        {"6011111111111117", "Stripe / Discover"},
        {"6011000990139424", "Stripe / Discover"},
        {"3056930009020004", "Stripe / Diners"},
        {"36227206271667",   "Stripe / Diners 14-digit"},
        {"3566002020360505", "Stripe / JCB"},
        {"6200000000000005", "Stripe / UnionPay"},
        {"4000000000009995", "Stripe / decline-insufficient_funds"},
        {"4000000000000002", "Stripe / decline-card_declined"},
        {"4000000000000069", "Stripe / decline-expired_card"},
        {"4000000000000127", "Stripe / decline-incorrect_cvc"},

        // Adyen: https://docs.adyen.com/development-resources/testing/test-card-numbers
        {"4111111111111111", "Adyen / Visa"},
        {"5454545454545454", "Adyen / Mastercard credit"},
        {"5500000000000004", "Adyen / Mastercard debit"},

        // Braintree: https://developer.paypal.com/braintree/docs/reference/general/testing/node
        {"4012000033330026", "Braintree / Visa"},
        {"4012000077777777", "Braintree / Visa"},
        {"4500600000000061", "Braintree / Visa debit"},
        {"5105105105105100", "Braintree / Mastercard"},

        // PayPal sandbox cards (public docs)
        {"4032035728492972", "PayPal / Visa sandbox"},
        {"5425233430109903", "PayPal / Mastercard sandbox"},

        // Square: https://developer.squareup.com/docs/devtools/sandbox/payments
        {"4111111111111111", "Square / Visa"},
        {"5105105105105100", "Square / Mastercard"},
    };
    return T;
}

// Returns the first matching label or empty.
inline std::string_view known_test_card_label(std::string_view digits) {
    for (const auto& [pan, label] : known_test_cards()) {
        if (digits == pan) return label;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct Report {
    std::vector<Issue> issues;
    std::string        normalized_number;
    const Scheme*      scheme = nullptr;
    int                detected_pan_length = 0;
    bool               luhn_ok = false;

    // Convenience accessors.
    bool has_errors()   const noexcept { return count(Severity::Error)   > 0; }
    bool has_warnings() const noexcept { return count(Severity::Warning) > 0; }
    bool ok()           const noexcept { return !has_errors(); }

    int count(Severity s) const noexcept {
        int n = 0; for (const auto& i : issues) if (i.severity == s) ++n; return n;
    }
};

struct ValidateInput {
    std::string_view  number;          // required (any human form)
    std::string_view  expiry;          // optional, MM/YY or empty
    int               month = 0;       // 1..12, 0 = take from `expiry`
    int               year_yy = -1;    // 0..99, -1 = take from `expiry`
    std::string_view  cvv;             // optional
    const Scheme*     expected_scheme = nullptr;        // if user claims one
    std::string_view  expected_country_iso2;            // optional
    CardType          expected_type = CardType::Unknown; // optional
    const BinDatabase* db = nullptr;   // optional, enables DB cross-checks
    Date              today = current_date();
    int               max_future_years = 12; // upper bound for plausible expiry
};

namespace detail {

inline void push(Report& r, Severity s, IssueCode c, std::string msg) {
    r.issues.push_back(Issue{s, c, std::move(msg)});
}

inline bool parse_mm_yy(std::string_view e, int& month, int& yy) {
    if (e.size() != 5 || e[2] != '/') return false;
    if (!all_digits(e.substr(0, 2)) || !all_digits(e.substr(3, 2))) return false;
    month = (e[0] - '0') * 10 + (e[1] - '0');
    yy    = (e[3] - '0') * 10 + (e[4] - '0');
    return true;
}

} // namespace detail

// The primary entry point. Returns a complete `Report` covering every
// dimension the offline validator can check. Never throws.
inline Report validate_full(const ValidateInput& in) {
    Report r;

    // ----- 1. Normalize number ---------------------------------------------
    auto norm = normalize_number_full(in.number);
    if (!norm.ok) {
        detail::push(r, Severity::Error, IssueCode::NonDigit,
                     "card number contains characters that aren't digits or "
                     "recognized separators");
        return r;
    }
    if (norm.digits.empty()) {
        detail::push(r, Severity::Error, IssueCode::Empty, "card number is empty");
        return r;
    }
    r.normalized_number = std::move(norm.digits);
    r.detected_pan_length = static_cast<int>(r.normalized_number.size());
    if (norm.stripped_separators) {
        detail::push(r, Severity::Info, IssueCode::NormalizedSeparators,
                     "stripped non-digit separators from input");
    }
    if (norm.mapped_unicode) {
        detail::push(r, Severity::Info, IssueCode::NonAsciiInputAccepted,
                     "mapped non-ASCII digit codepoints to ASCII");
    }

    // ----- 2. Scheme detection ---------------------------------------------
    uint8_t match_len = 0;
    r.scheme = detect_scheme(r.normalized_number, &match_len);
    if (in.expected_scheme) {
        const uint8_t exp_match = scheme_match_len(*in.expected_scheme, r.normalized_number);
        if (exp_match == 0) {
            detail::push(r, Severity::Error, IssueCode::SchemeMismatch,
                         std::string("BIN does not match expected scheme '") +
                         std::string(in.expected_scheme->name) + "'");
        } else if (in.expected_scheme != r.scheme) {
            // The user-claimed scheme matches, but a more specific one also
            // claims this prefix. Honour the user's intent for downstream
            // length/CVV checks but warn about the ambiguity.
            r.scheme = in.expected_scheme;
            detail::push(r, Severity::Warning, IssueCode::AmbiguousScheme,
                         "BIN is claimed by multiple schemes; using the one supplied");
        }
    } else if (!r.scheme) {
        detail::push(r, Severity::Error, IssueCode::UnknownScheme,
                     "BIN does not fall in any known scheme range");
    } else {
        // Detect ambiguous prefix (multiple schemes match at the same depth).
        auto matchers = all_matching_schemes(r.normalized_number);
        if (matchers.size() > 1) {
            std::string names;
            for (size_t i = 0; i < matchers.size(); ++i) {
                if (i) names += ", ";
                names += std::string(matchers[i]->name);
            }
            detail::push(r, Severity::Warning, IssueCode::AmbiguousScheme,
                         "BIN matches multiple schemes (" + names + "); picked '" +
                         std::string(r.scheme->name) + "'");
        }
    }

    // ----- 3. Length -------------------------------------------------------
    if (r.scheme) {
        if (!r.scheme->accepts_length(r.detected_pan_length)) {
            std::string msg = std::string("PAN length ") + std::to_string(r.detected_pan_length) +
                              " is not accepted by " + std::string(r.scheme->name) + " (";
            for (size_t i = 0; i < r.scheme->lengths.size(); ++i) {
                if (i) msg += "/";
                msg += std::to_string(r.scheme->lengths[i]);
            }
            msg += ")";
            detail::push(r, Severity::Error, IssueCode::BadLength, std::move(msg));
        }
    }

    // ----- 4. Luhn ---------------------------------------------------------
    r.luhn_ok = luhn_check(r.normalized_number);
    if (r.scheme && r.scheme->luhn_required && !r.luhn_ok) {
        detail::push(r, Severity::Error, IssueCode::BadLuhn,
                     "Luhn checksum is invalid");
    }

    // ----- 5. Anti-patterns (warnings, not errors) -------------------------
    auto stats = analyze_digits(r.normalized_number);
    if (stats.all_same) {
        detail::push(r, Severity::Warning, IssueCode::AllSameDigit,
                     "all digits are identical");
    } else {
        if (stats.longest_run >= 6) {
            detail::push(r, Severity::Warning, IssueCode::LowEntropy,
                         "long run of repeated digits (" +
                         std::to_string(stats.longest_run) + ")");
        }
        if (stats.longest_seq >= 6) {
            detail::push(r, Severity::Warning, IssueCode::SequentialDigits,
                         "long monotonic sequence of digits (" +
                         std::to_string(stats.longest_seq) + ")");
        }
        if (stats.unique_digits <= 2 && r.detected_pan_length >= 13) {
            detail::push(r, Severity::Warning, IssueCode::LowEntropy,
                         "uses only " + std::to_string(stats.unique_digits) +
                         " distinct digit value(s)");
        }
    }

    // ----- 6. Known test PAN -----------------------------------------------
    if (auto label = known_test_card_label(r.normalized_number); !label.empty()) {
        detail::push(r, Severity::Warning, IssueCode::KnownTestCard,
                     std::string("recognized published test card (") +
                     std::string(label) + ")");
    }

    // ----- 7. Expiry -------------------------------------------------------
    int month = in.month, yy = in.year_yy;
    bool has_expiry = (month > 0 || yy >= 0 || !in.expiry.empty());
    if (!in.expiry.empty()) {
        int m = 0, y = 0;
        if (!detail::parse_mm_yy(in.expiry, m, y)) {
            detail::push(r, Severity::Error, IssueCode::BadMonth,
                         "expiry must be MM/YY");
            has_expiry = false;
        } else {
            month = m;
            yy    = y;
        }
    }
    if (has_expiry) {
        if (month < 1 || month > 12) {
            detail::push(r, Severity::Error, IssueCode::BadMonth,
                         "expiry month must be 1..12");
        }
        if (yy < 0 || yy > 99) {
            detail::push(r, Severity::Error, IssueCode::BadYear,
                         "expiry year must be 00..99");
        }
        if (month >= 1 && month <= 12 && yy >= 0 && yy <= 99) {
            const int full = resolve_full_year(yy, in.today.year);
            if (full < in.today.year ||
                (full == in.today.year && month < in.today.month)) {
                detail::push(r, Severity::Error, IssueCode::Expired,
                             "card is expired");
            } else if (full > in.today.year + in.max_future_years) {
                detail::push(r, Severity::Warning, IssueCode::ImplausibleFutureExpiry,
                             "expiry is more than " +
                             std::to_string(in.max_future_years) +
                             " years in the future");
            }
        }
    }

    // ----- 8. CVV ----------------------------------------------------------
    if (!in.cvv.empty()) {
        for (char c : in.cvv) {
            if (c < '0' || c > '9') {
                detail::push(r, Severity::Error, IssueCode::BadCvvDigits,
                             "CVV must contain only digits");
                break;
            }
        }
        if (r.scheme && !r.scheme->accepts_cvv_length(static_cast<int>(in.cvv.size()))) {
            std::string msg = "CVV length " + std::to_string(in.cvv.size()) +
                              " is not accepted by " + std::string(r.scheme->name) + " (";
            for (size_t i = 0; i < r.scheme->cvv_lengths.size(); ++i) {
                if (i) msg += "/";
                msg += std::to_string(r.scheme->cvv_lengths[i]);
            }
            msg += ")";
            detail::push(r, Severity::Error, IssueCode::BadCvvLength, std::move(msg));
        }
        // Always remind callers we cannot verify the value cryptographically.
        detail::push(r, Severity::Info, IssueCode::CvkNotChecked,
                     "CVV format is OK; cryptographic CVK verification requires "
                     "the issuer's key and is not done offline");
    }

    // ----- 9. Cross-check with BIN database --------------------------------
    if (in.db && !in.db->empty() && r.normalized_number.size() >= 6) {
        const std::string bin6 = r.normalized_number.substr(0, 6);
        const auto* info = in.db->lookup(bin6);
        if (!info) {
            detail::push(r, Severity::Warning, IssueCode::DbBinUnknown,
                         "BIN " + bin6 + " is not present in the loaded BIN database");
        } else {
            // Scheme cross-check.
            if (r.scheme) {
                const Scheme* db_scheme = nullptr;
                std::string brand_lower(info->brand);
                std::transform(brand_lower.begin(), brand_lower.end(),
                               brand_lower.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                // Map the upstream "Brand" string to one of our schemes.
                static const std::pair<std::string_view, std::string_view> map[] = {
                    {"visa",                       "visa"},
                    {"mastercard",                 "mastercard"},
                    {"american express",           "amex"},
                    {"discover",                   "discover"},
                    {"jcb",                        "jcb"},
                    {"diners club international",  "diners"},
                    {"diners club",                "diners"},
                    {"maestro",                    "maestro"},
                    {"china union pay",            "unionpay"},
                    {"unionpay",                   "unionpay"},
                    {"nspk mir",                   "mir"},
                    {"rupay",                      "rupay"},
                    {"jcb/rupay",                  "rupay"},
                    {"verve",                      "verve"},
                    {"hipercard",                  "hipercard"},
                    {"dankort",                    "dankort"},
                };
                for (const auto& [from, to] : map) {
                    if (brand_lower == from) { db_scheme = find_scheme(to); break; }
                }
                if (db_scheme && db_scheme != r.scheme) {
                    detail::push(r, Severity::Warning, IssueCode::DbSchemeDisagrees,
                                 std::string("BIN database lists scheme as '") +
                                 std::string(info->brand) + "', detected '" +
                                 std::string(r.scheme->name) + "'");
                }
            }
            // Country cross-check.
            if (!in.expected_country_iso2.empty() &&
                !info->iso_alpha2.empty() &&
                in.expected_country_iso2 != info->iso_alpha2) {
                detail::push(r, Severity::Warning, IssueCode::DbCountryDisagrees,
                             std::string("expected country ") +
                             std::string(in.expected_country_iso2) +
                             " but BIN database lists " +
                             std::string(info->iso_alpha2));
            }
            // Type cross-check.
            if (in.expected_type != CardType::Unknown &&
                info->type != CardType::Unknown &&
                info->type != in.expected_type) {
                detail::push(r, Severity::Warning, IssueCode::DbTypeDisagrees,
                             std::string("expected type ") + to_string(in.expected_type) +
                             " but BIN database lists " + to_string(info->type));
            }
        }
    }

    return r;
}

// Render a Report to a single ANSI-friendly text block. Compact layout meant
// for terminal use; JSON serialization lives in main.cpp.
inline std::string render_text(const Report& r) {
    std::string out;
    out += r.ok() ? "VALID" : "INVALID";
    if (r.has_warnings()) out += " (with warnings)";
    out += "\n";
    if (!r.normalized_number.empty()) {
        out += "  number:        " + r.normalized_number + "\n";
        out += "  length:        " + std::to_string(r.detected_pan_length) + "\n";
    }
    if (r.scheme) {
        out += "  scheme:        ";
        out.append(r.scheme->display.data(), r.scheme->display.size());
        out += " (lengths ";
        for (size_t i = 0; i < r.scheme->lengths.size(); ++i) {
            if (i) out += "/";
            out += std::to_string(r.scheme->lengths[i]);
        }
        out += ", CVV ";
        for (size_t i = 0; i < r.scheme->cvv_lengths.size(); ++i) {
            if (i) out += "/";
            out += std::to_string(r.scheme->cvv_lengths[i]);
        }
        out += ")\n";
    }
    out += "  luhn:          ";
    out += r.luhn_ok ? "ok" : "FAIL";
    out += "\n";
    if (!r.issues.empty()) {
        out += "  issues:\n";
        for (const auto& i : r.issues) {
            out += "    [";
            out += to_string(i.severity);
            out += " ";
            out += to_string(i.code);
            out += "] ";
            out += i.message;
            out += "\n";
        }
    }
    return out;
}

} // namespace cards
