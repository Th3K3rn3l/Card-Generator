#include "bin_db.hpp"
#include "cards.hpp"
#include "validator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static void print_usage(std::FILE* out) {
    std::fprintf(out,
        "Usage:\n"
        "  generator <count> [options]                  Generate cards (default).\n"
        "  generator --check  <num> [<MM/YY> <CVV>]     Validate a card / number.\n"
        "  generator --lookup <BIN>                     Print BIN database info.\n"
        "  generator --fuzz   <count> [options]         Fuzz the validator.\n"
        "\n"
        "Generates <count> Luhn-valid synthetic card records.\n"
        "Default line: <card-number> <MM/YY> <CVV>\n"
        "\n"
        "Brand & prefix:\n"
        "  -b, --brand <name>     visa | mc | amex | discover | jcb (default: visa)\n"
        "      --bin <digits>     Custom BIN/IIN prefix. Brand is auto-detected\n"
        "                         from this BIN unless --brand is set explicitly.\n"
        "                         If both are given, they must agree.\n"
        "      --length <N>       Override card length (8..%d).\n"
        "      --cvv-length <N>   Override CVV length (1..%d).\n"
        "      --no-luhn          Do not enforce Luhn checksum (last digit random).\n"
        "\n"
        "Fixed fields:\n"
        "      --expiry <MM/YY>   Same expiry date for every card.\n"
        "      --month <1..12>    Fix only the month, keep year random.\n"
        "      --year <YY|YYYY>   Fix only the year, keep month random.\n"
        "      --year-min <YY>    Lower bound for random year (2-digit).\n"
        "      --year-max <YY>    Upper bound for random year (2-digit).\n"
        "      --year-span <N>    Years above current for the random year (default 6).\n"
        "      --cvv <NNN[N]>     Same CVV for every card.\n"
        "      --allow-expired    Allow --expiry / --year in the past (default: error).\n"
        "\n"
        "Output:\n"
        "      --no-format        Do not insert spaces between groups of 4 digits.\n"
        "      --sep <s>          Field separator (default: \" \").\n"
        "      --json             JSON Lines: {\"number\":...,\"expiry\":...,\"cvv\":...}.\n"
        "      --csv              Shortcut: --sep ',' --no-format --header.\n"
        "      --header           Emit a 'number<sep>expiry<sep>cvv' header line first.\n"
        "      --info             Append BIN info (bank, type, country) to each line.\n"
        "                         Requires the BIN database (see below).\n"
        "\n"
        "BIN database (optional, used by --info, --lookup and --check):\n"
        "      --bin-db <path>    Path to a CSV BIN database. If omitted, falls back\n"
        "                         to $CARD_GENERATOR_BIN_DB or ./data/bin-list-data.csv.\n"
        "                         Schema: BIN,Brand,Type,Category,Issuer,IssuerPhone,\n"
        "                                 IssuerUrl,isoCode2,isoCode3,CountryName.\n"
        "\n"
        "Validation (--check):\n"
        "      --check <num> [MM/YY] [CVV]    Validate a card. Accepts spaces, dashes,\n"
        "                                     non-ASCII digits and Unicode separators.\n"
        "      --check-json                   Emit the full report as JSON.\n"
        "      --strict-warnings              Treat warnings as failures (exit 1).\n"
        "      --expect-country <ISO2>        Cross-check country with BIN database.\n"
        "      --expect-type <credit|debit|prepaid|charge>\n"
        "                                     Cross-check card type with BIN database.\n"
        "      --no-bin-db                    Skip BIN-database cross-checks.\n"
        "\n"
        "Fuzzing (--fuzz, for stress-testing the validator):\n"
        "      --fuzz <count>     Run `count` random validation attempts and print\n"
        "                         a histogram of issues. Useful for benchmarking.\n"
        "\n"
        "Reproducibility:\n"
        "      --seed <N>         Deterministic 64-bit seed (any decimal value).\n"
        "\n"
        "Misc:\n"
        "  -h, --help             Show this help.\n"
        "\n"
        "Examples:\n"
        "  generator 100\n"
        "  generator 1000 --brand mc\n"
        "  generator 100 --bin 414720\n"
        "  generator 100 --info               # appends bank/type/country to each line\n"
        "  generator --lookup 414720          # show BIN database entry\n"
        "  generator --check 4929923142719262 12/30 310\n"
        "  generator --check 4242424242424242 --check-json\n"
        "  generator --fuzz 10000 --seed 7\n"
        "  generator 50 --expiry 12/30 --cvv 123\n"
        "  generator 100 --year 2030 --month 6\n"
        "  generator 1000 --csv > cards.csv\n"
        "  generator 1000 --json > cards.jsonl\n"
        "  generator 5 --seed 42        # same seed = identical output\n",
        cards::MAX_NUMBER_DIGITS, cards::MAX_CVV_DIGITS);
}

static bool parse_int(const char* s, long long& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long long v = std::strtoll(s, &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

static bool parse_expiry(std::string_view e, int& month, int& yy) {
    if (e.size() != 5 || e[2] != '/') return false;
    if (!cards::all_digits(e.substr(0, 2)) || !cards::all_digits(e.substr(3, 2))) return false;
    month = (e[0] - '0') * 10 + (e[1] - '0');
    yy = (e[3] - '0') * 10 + (e[4] - '0');
    return month >= 1 && month <= 12;
}

// ---------------------------------------------------------------------------
// BIN info formatting
// ---------------------------------------------------------------------------

namespace {

void append_field(std::string& dst, const std::string& sep, std::string_view value,
                  std::string_view fallback) {
    dst += sep;
    if (!value.empty()) dst.append(value.data(), value.size());
    else                dst.append(fallback.data(), fallback.size());
}

// Strip surrounding ASCII whitespace.
std::string_view trim(std::string_view s) {
    size_t i = 0, j = s.size();
    while (i < j && (s[i] == ' ' || s[i] == '\t')) ++i;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
    return s.substr(i, j - i);
}

// Renders the BIN-database info as a single-line "key=value" suffix joined by
// the user's separator. If `out_format == Json`, emits an extension JSON
// fragment that gets glued onto the generator's JSON line.
std::string format_bin_info(const cards::BinInfo* info,
                            const std::string& sep,
                            cards::OutputFormat out_format) {
    if (!info) {
        if (out_format == cards::OutputFormat::Json) {
            return ",\"bin_info\":null";
        }
        std::string s;
        s += sep;
        s += "bin=unknown";
        return s;
    }

    if (out_format == cards::OutputFormat::Json) {
        // We deliberately keep it small and stable: bank, type, country.
        std::string s;
        s += ",\"bank\":\"";
        s.append(info->issuer.data(), info->issuer.size());
        s += "\",\"type\":\"";
        s += cards::to_string(info->type);
        s += "\",\"country\":\"";
        s.append(info->country.data(), info->country.size());
        s += "\"";
        // Optional fields go behind a stable schema, only emitted if present.
        if (!info->category.empty()) {
            s += ",\"category\":\"";
            s.append(info->category.data(), info->category.size());
            s += "\"";
        }
        if (!info->iso_alpha2.empty()) {
            s += ",\"iso2\":\"";
            s.append(info->iso_alpha2.data(), info->iso_alpha2.size());
            s += "\"";
        }
        return s;
    }

    std::string s;
    append_field(s, sep, info->issuer, "unknown bank");
    append_field(s, sep, cards::to_string(info->type), "unknown");
    append_field(s, sep, info->country, "unknown country");
    if (!info->category.empty()) append_field(s, sep, info->category, "");
    return s;
}

// Renders `key=value` with a separator between fields, used by --lookup.
void print_lookup(std::FILE* out, std::string_view bin, const cards::BinInfo* info) {
    if (!info) {
        std::fprintf(out, "BIN %.*s: not found in database\n",
                     static_cast<int>(bin.size()), bin.data());
        return;
    }
    std::fprintf(out, "BIN:        %06u\n", info->bin);
    std::fprintf(out, "Brand:      %.*s\n", static_cast<int>(info->brand.size()), info->brand.data());
    std::fprintf(out, "Type:       %s\n", cards::to_string(info->type));
    if (!info->category.empty())
        std::fprintf(out, "Category:   %.*s\n", static_cast<int>(info->category.size()), info->category.data());
    if (!info->issuer.empty())
        std::fprintf(out, "Issuer:     %.*s\n", static_cast<int>(info->issuer.size()), info->issuer.data());
    if (!info->issuer_phone.empty())
        std::fprintf(out, "Phone:      %.*s\n", static_cast<int>(info->issuer_phone.size()), info->issuer_phone.data());
    if (!info->issuer_url.empty())
        std::fprintf(out, "URL:        %.*s\n", static_cast<int>(info->issuer_url.size()), info->issuer_url.data());
    if (!info->country.empty())
        std::fprintf(out, "Country:    %.*s (%.*s)\n",
                     static_cast<int>(info->country.size()), info->country.data(),
                     static_cast<int>(info->iso_alpha2.size()), info->iso_alpha2.data());
}

bool ensure_bin_db(cards::BinDatabase& db, const std::string& explicit_path,
                   bool warn_if_missing) {
    if (!db.empty()) return true;
    std::string path = explicit_path.empty() ? cards::default_bin_db_path()
                                             : explicit_path;
    if (path.empty()) {
        if (warn_if_missing) {
            std::fprintf(stderr,
                "generator: BIN database not found.\n"
                "  Pass --bin-db <path>, set $CARD_GENERATOR_BIN_DB,\n"
                "  or place the CSV at ./data/bin-list-data.csv.\n");
        }
        return false;
    }
    if (!db.load(path)) {
        std::fprintf(stderr, "generator: failed to read BIN database: %s\n", path.c_str());
        return false;
    }
    return true;
}

// Subcommand: validate a single user-supplied card. Returns the process
// exit code: 0 = valid, 1 = invalid (or warnings if --strict-warnings),
// 2 = bad arguments.
int run_check(int argc, char** argv) {
    int positional = 0;
    std::string number, expiry, cvv;
    std::string bin_db_path;
    std::string expect_country;
    cards::CardType expect_type = cards::CardType::Unknown;
    const cards::Scheme* expected_scheme = nullptr;
    bool emit_json = false;
    bool strict_warnings = false;
    bool use_db = true;

    auto type_from = [](std::string_view s) {
        if (s == "credit")  return cards::CardType::Credit;
        if (s == "debit")   return cards::CardType::Debit;
        if (s == "prepaid") return cards::CardType::Prepaid;
        if (s == "charge")  return cards::CardType::Charge;
        return cards::CardType::Unknown;
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--bin-db" && i + 1 < argc)            { bin_db_path = argv[++i]; continue; }
        if ((a == "-b" || a == "--brand") && i + 1 < argc) {
            std::string_view name = argv[++i];
            // Map the legacy brand name to a scheme name.
            if (name == "mc") name = "mastercard";
            expected_scheme = cards::find_scheme(name);
            if (!expected_scheme) {
                std::fprintf(stderr, "generator: unknown scheme: %.*s\n",
                             static_cast<int>(name.size()), name.data());
                return 2;
            }
            continue;
        }
        if (a == "--check-json")        { emit_json = true; continue; }
        if (a == "--strict-warnings")   { strict_warnings = true; continue; }
        if (a == "--no-bin-db")         { use_db = false; continue; }
        if (a == "--expect-country" && i + 1 < argc) { expect_country = argv[++i]; continue; }
        if (a == "--expect-type"    && i + 1 < argc) {
            expect_type = type_from(argv[++i]);
            if (expect_type == cards::CardType::Unknown) {
                std::fprintf(stderr, "generator: --expect-type must be credit|debit|prepaid|charge\n");
                return 2;
            }
            continue;
        }
        if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        if (a.size() > 2 && a[0] == '-') {
            std::fprintf(stderr, "generator: --check: unknown option: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        switch (positional++) {
            case 0: number = std::string(a); break;
            case 1: expiry = std::string(a); break;
            case 2: cvv    = std::string(a); break;
            default:
                std::fprintf(stderr, "generator: --check: unexpected extra argument: %.*s\n",
                             static_cast<int>(a.size()), a.data());
                return 2;
        }
    }
    if (number.empty()) {
        std::fprintf(stderr, "generator: --check: missing card number\n");
        return 2;
    }

    // Load BIN database if available; pass nullptr if disabled or missing.
    cards::BinDatabase db;
    if (use_db) ensure_bin_db(db, bin_db_path, /*warn_if_missing=*/false);

    cards::ValidateInput in;
    in.number = number;
    in.expiry = expiry;
    in.cvv    = cvv;
    in.expected_scheme = expected_scheme;
    in.expected_country_iso2 = expect_country;
    in.expected_type = expect_type;
    in.db = (db.empty() ? nullptr : &db);

    cards::Report rep = cards::validate_full(in);

    if (emit_json) {
        // Compact JSON serializer; we keep it inline rather than adding a
        // dependency. Every string field is JSON-escaped.
        auto esc = [](std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else out += c;
                }
            }
            return out;
        };
        std::printf("{\"valid\":%s,\"warnings\":%d,\"errors\":%d",
                    rep.ok() ? "true" : "false",
                    rep.count(cards::Severity::Warning),
                    rep.count(cards::Severity::Error));
        std::printf(",\"number\":\"%s\"", esc(rep.normalized_number).c_str());
        std::printf(",\"length\":%d", rep.detected_pan_length);
        if (rep.scheme) std::printf(",\"scheme\":\"%s\"", esc(rep.scheme->name).c_str());
        std::printf(",\"luhn\":%s", rep.luhn_ok ? "true" : "false");
        std::printf(",\"issues\":[");
        for (size_t i = 0; i < rep.issues.size(); ++i) {
            if (i) std::printf(",");
            std::printf("{\"severity\":\"%s\",\"code\":\"%s\",\"message\":\"%s\"}",
                        cards::to_string(rep.issues[i].severity),
                        cards::to_string(rep.issues[i].code),
                        esc(rep.issues[i].message).c_str());
        }
        std::printf("]}\n");
    } else {
        std::string text = cards::render_text(rep);
        std::fwrite(text.data(), 1, text.size(), stdout);

        // Always print BIN-DB record if we have one; reusing the existing
        // pretty-printer keeps the output familiar.
        if (!db.empty() && rep.normalized_number.size() >= 6) {
            std::string bin = rep.normalized_number.substr(0, 6);
            if (const auto* info = db.lookup(bin)) {
                std::printf("\n");
                print_lookup(stdout, bin, info);
            }
        }
    }

    if (!rep.ok())                          return 1;
    if (strict_warnings && rep.has_warnings()) return 1;
    return 0;
}

// Subcommand: pretty-print BIN info for a user-supplied prefix.
int run_lookup(int argc, char** argv) {
    std::string bin;
    std::string bin_db_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--bin-db" && i + 1 < argc) { bin_db_path = argv[++i]; continue; }
        if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        if (a.size() > 2 && a[0] == '-') {
            std::fprintf(stderr, "generator: --lookup: unknown option: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        if (bin.empty()) bin.assign(a);
        else {
            std::fprintf(stderr, "generator: --lookup: too many arguments\n");
            return 2;
        }
    }
    if (bin.empty()) { std::fprintf(stderr, "generator: --lookup: missing BIN\n"); return 2; }
    // Allow user-pasted "4929 9231 ..." style numbers — strip separators.
    auto norm = cards::normalize_number(bin);
    if (!norm.ok || norm.digits.empty()) {
        std::fprintf(stderr, "generator: --lookup: BIN must be digits\n");
        return 2;
    }
    if (norm.digits.size() > 6) norm.digits.resize(6);

    cards::BinDatabase db;
    if (!ensure_bin_db(db, bin_db_path, /*warn_if_missing=*/true)) return 2;
    const auto* info = db.lookup(norm.digits);
    print_lookup(stdout, norm.digits, info);
    return info ? 0 : 1;
}

// Subcommand: stress-test the validator. Generates `count` synthetic cards
// (some clean, some intentionally corrupted), runs validate_full on each and
// prints latency + an issue histogram. Useful to demonstrate the validator's
// coverage and throughput in a contest setting.
int run_fuzz(int argc, char** argv) {
    long long count = 0;
    uint64_t seed = 0;
    bool seed_set = false;
    std::string bin_db_path;
    bool use_db = true;

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--seed" && i + 1 < argc) {
            seed = static_cast<uint64_t>(std::strtoll(argv[++i], nullptr, 10));
            seed_set = true;
            continue;
        }
        if (a == "--bin-db" && i + 1 < argc) { bin_db_path = argv[++i]; continue; }
        if (a == "--no-bin-db")              { use_db = false; continue; }
        if (a == "-h" || a == "--help")      { print_usage(stdout); return 0; }
        if (a.size() > 2 && a[0] == '-') {
            std::fprintf(stderr, "generator: --fuzz: unknown option: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        if (count == 0) {
            char* e = nullptr;
            count = std::strtoll(argv[i], &e, 10);
            if (!e || *e != '\0' || count <= 0) {
                std::fprintf(stderr, "generator: --fuzz: bad count: %s\n", argv[i]);
                return 2;
            }
        } else {
            std::fprintf(stderr, "generator: --fuzz: unexpected extra argument: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    if (count == 0) { std::fprintf(stderr, "generator: --fuzz: missing count\n"); return 2; }

    cards::BinDatabase db;
    if (use_db) ensure_bin_db(db, bin_db_path, /*warn_if_missing=*/false);
    const cards::BinDatabase* db_ptr = db.empty() ? nullptr : &db;

    cards::Xoshiro rng;
    if (seed_set) rng.seed(seed ? seed : 1);
    else          rng.seed_random();

    // Pull one schema each from the schemes table and use them in rotation,
    // so the fuzzer exercises Visa/MC/Amex/Discover/JCB/Maestro paths in
    // proportion. We bias toward the four most common schemes.
    static const std::array<const char*, 6> schemes_to_test = {
        "visa", "mastercard", "amex", "discover", "jcb", "maestro"
    };

    std::array<long long, 32> by_severity_count{};
    std::array<std::pair<cards::IssueCode, long long>, 24> hist{};
    auto bump = [&](cards::IssueCode c) {
        for (auto& h : hist) {
            if (h.second == 0)            { h.first = c; h.second = 1; return; }
            if (h.first == c)             { h.second += 1; return; }
        }
        // Histogram full; ignore (shouldn't happen — there are 24 codes).
    };

    auto t0 = std::chrono::high_resolution_clock::now();
    long long valid = 0, invalid = 0, with_warnings = 0;

    cards::Options opt;
    for (long long n = 0; n < count; ++n) {
        const auto* scheme_name = schemes_to_test[n % schemes_to_test.size()];
        // Map scheme → generator brand. We only ship a few brands in the
        // generator's table, so we reuse "visa" as fallback for the rest.
        const cards::Brand* brand =
              cards::find_brand(scheme_name)
            ? cards::find_brand(scheme_name)
            : cards::find_brand((std::string_view(scheme_name) == "mastercard") ? "mc" : "visa");
        opt.brand = brand;
        opt.format = false;
        opt.enforce_luhn = (cards::bounded(rng, 100) >= 5); // 5% bad-Luhn

        std::string buf;
        buf.reserve(64);
        cards::generate(1, opt, [&](const char* p, std::size_t sz){ buf.append(p, sz); });

        // Buffer holds "PAN MM/YY CVV\n". Split it cheaply.
        size_t sp1 = buf.find(' ');
        size_t sp2 = buf.find(' ', sp1 + 1);
        size_t nl  = buf.find('\n');
        if (sp1 == std::string::npos || sp2 == std::string::npos || nl == std::string::npos) continue;
        std::string num    = buf.substr(0,     sp1);
        std::string expiry = buf.substr(sp1+1, sp2 - sp1 - 1);
        std::string cvv    = buf.substr(sp2+1, nl  - sp2 - 1);

        // Inject random damage.
        const uint32_t damage = cards::bounded(rng, 100);
        if (damage < 5) num[cards::bounded(rng, static_cast<uint32_t>(num.size()))]
                        = static_cast<char>('0' + (cards::bounded(rng, 10)));
        else if (damage < 8) num.pop_back();           // truncate
        else if (damage < 11) num += "0";              // overlong
        else if (damage < 13) cvv += "0";              // bad CVV length
        else if (damage < 15) expiry = "13/30";        // bad month

        cards::ValidateInput in;
        in.number = num;
        in.expiry = expiry;
        in.cvv    = cvv;
        in.db     = db_ptr;
        cards::Report rep = cards::validate_full(in);

        if (rep.ok())            ++valid; else ++invalid;
        if (rep.has_warnings())  ++with_warnings;
        for (const auto& iss : rep.issues) {
            by_severity_count[static_cast<size_t>(iss.severity)]++;
            bump(iss.code);
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double per_sec = count / (ms / 1000.0);

    std::printf("fuzzed   %lld validations\n", count);
    std::printf("elapsed  %.2f ms (%.0f /sec)\n", ms, per_sec);
    std::printf("valid    %lld (%.1f%%)\n", valid, 100.0 * valid    / count);
    std::printf("invalid  %lld (%.1f%%)\n", invalid, 100.0 * invalid / count);
    std::printf("warnings %lld (%.1f%%)\n", with_warnings, 100.0 * with_warnings / count);
    std::printf("by severity:\n");
    std::printf("  errors    %lld\n", by_severity_count[static_cast<size_t>(cards::Severity::Error)]);
    std::printf("  warnings  %lld\n", by_severity_count[static_cast<size_t>(cards::Severity::Warning)]);
    std::printf("  info      %lld\n", by_severity_count[static_cast<size_t>(cards::Severity::Info)]);
    std::printf("histogram:\n");
    // Sort histogram entries by count, descending.
    std::vector<std::pair<cards::IssueCode, long long>> sorted;
    for (auto& h : hist) if (h.second) sorted.push_back(h);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });
    for (const auto& h : sorted) {
        std::printf("  %-26s %8lld\n", cards::to_string(h.first), h.second);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Subcommand dispatch first — they short-circuit the rest.
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--check" || a == "--lookup" || a == "--fuzz") {
            // Build a contiguous argv starting at the subcommand for the helper.
            int sub_argc = argc - i;
            char** sub_argv = argv + i;
            if (a == "--check")  return run_check(sub_argc, sub_argv);
            if (a == "--lookup") return run_lookup(sub_argc, sub_argv);
            return run_fuzz(sub_argc, sub_argv);
        }
        if (a == "--") break; // explicit end of option parsing
    }

    long long count = 0;
    cards::Options opt;
    opt.brand = cards::find_brand("visa");
    bool brand_explicit = false;
    bool with_info = false;
    bool allow_expired = false;
    std::string bin_db_path;

    auto need = [&](int& i, const char* name) -> const char* {
        if (++i >= argc) {
            std::fprintf(stderr, "generator: missing value for %s\n", name);
            std::exit(2);
        }
        return argv[i];
    };

    for (int i = 1; i < argc; i++) {
        std::string_view a(argv[i]);

        if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else if (a == "-b" || a == "--brand") {
            const char* v = need(i, "--brand");
            const auto* b = cards::find_brand(v);
            if (!b) { std::fprintf(stderr, "generator: unknown brand: %s\n", v); return 2; }
            opt.brand = b; brand_explicit = true;
        }
        else if (a == "--bin") {
            std::string_view bin(need(i, "--bin"));
            if (!cards::all_digits(bin)) {
                std::fprintf(stderr, "generator: --bin must be digits only\n"); return 2;
            }
            opt.custom_prefix.assign(bin);
        }
        else if (a == "--length") {
            long long v; if (!parse_int(need(i, "--length"), v) || v < 8 || v > cards::MAX_NUMBER_DIGITS) {
                std::fprintf(stderr, "generator: --length must be 8..%d\n", cards::MAX_NUMBER_DIGITS); return 2;
            }
            opt.length_override = static_cast<int>(v);
        }
        else if (a == "--cvv-length") {
            long long v; if (!parse_int(need(i, "--cvv-length"), v) || v < 1 || v > cards::MAX_CVV_DIGITS) {
                std::fprintf(stderr, "generator: --cvv-length must be 1..%d\n", cards::MAX_CVV_DIGITS); return 2;
            }
            opt.cvv_length_override = static_cast<int>(v);
        }
        else if (a == "--no-luhn") {
            opt.enforce_luhn = false;
        }
        else if (a == "--expiry") {
            std::string_view e(need(i, "--expiry"));
            int m, y; if (!parse_expiry(e, m, y)) {
                std::fprintf(stderr, "generator: --expiry must be MM/YY\n"); return 2;
            }
            opt.fixed_month = m; opt.fixed_year = y;
        }
        else if (a == "--month") {
            long long v; if (!parse_int(need(i, "--month"), v) || v < 1 || v > 12) {
                std::fprintf(stderr, "generator: --month must be 1..12\n"); return 2;
            }
            opt.fixed_month = static_cast<int>(v);
        }
        else if (a == "--year") {
            long long v; if (!parse_int(need(i, "--year"), v) || v < 0 || v > 9999) {
                std::fprintf(stderr, "generator: --year must be 0..9999\n"); return 2;
            }
            opt.fixed_year = static_cast<int>(v % 100);
        }
        else if (a == "--year-min") {
            long long v; if (!parse_int(need(i, "--year-min"), v)) {
                std::fprintf(stderr, "generator: --year-min must be integer\n"); return 2;
            }
            opt.year_min = static_cast<int>(((v % 100) + 100) % 100);
        }
        else if (a == "--year-max") {
            long long v; if (!parse_int(need(i, "--year-max"), v)) {
                std::fprintf(stderr, "generator: --year-max must be integer\n"); return 2;
            }
            opt.year_max = static_cast<int>(((v % 100) + 100) % 100);
        }
        else if (a == "--year-span") {
            long long v; if (!parse_int(need(i, "--year-span"), v) || v < 1) {
                std::fprintf(stderr, "generator: --year-span must be >= 1\n"); return 2;
            }
            opt.year_span = static_cast<int>(v);
        }
        else if (a == "--cvv") {
            std::string_view c(need(i, "--cvv"));
            if (!cards::all_digits(c) || c.size() > cards::MAX_CVV_DIGITS) {
                std::fprintf(stderr, "generator: --cvv must be 1..%d digits\n", cards::MAX_CVV_DIGITS); return 2;
            }
            opt.fixed_cvv.assign(c);
        }
        else if (a == "--allow-expired") { allow_expired = true; }
        else if (a == "--no-format") { opt.format = false; }
        else if (a == "--sep") { opt.separator = need(i, "--sep"); }
        else if (a == "--json") { opt.output = cards::OutputFormat::Json; }
        else if (a == "--csv") {
            opt.output = cards::OutputFormat::Csv;
            opt.separator = ",";
            opt.format = false;
            opt.header = true;
        }
        else if (a == "--header") { opt.header = true; }
        else if (a == "--info")   { with_info = true; }
        else if (a == "--bin-db") { bin_db_path = need(i, "--bin-db"); }
        else if (a == "--seed") {
            long long v; if (!parse_int(need(i, "--seed"), v)) {
                std::fprintf(stderr, "generator: --seed must be integer\n"); return 2;
            }
            opt.seed = static_cast<uint64_t>(v);
            opt.seed_set = true;
        }
        else if (count == 0) {
            long long v; if (!parse_int(argv[i], v) || v <= 0) {
                std::fprintf(stderr, "generator: invalid count: %s\n", argv[i]); return 2;
            }
            count = v;
        } else {
            std::fprintf(stderr, "generator: unknown argument: %s\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (count == 0) { print_usage(stderr); return 1; }

    // Brand auto-detection from --bin, with consistency check when both
    // --brand and --bin are supplied explicitly.
    if (!opt.custom_prefix.empty()) {
        const auto* detected = cards::detect_brand_from_bin(opt.custom_prefix);
        if (brand_explicit) {
            if (!cards::brand_matches_prefix(opt.brand, opt.custom_prefix)) {
                std::fprintf(stderr,
                    "generator: --brand %.*s does not match --bin %s",
                    static_cast<int>(opt.brand->name.size()), opt.brand->name.data(),
                    opt.custom_prefix.c_str());
                if (detected) {
                    std::fprintf(stderr, " (BIN looks like %.*s)",
                                 static_cast<int>(detected->name.size()), detected->name.data());
                }
                std::fprintf(stderr, "\n");
                return 2;
            }
        } else if (detected) {
            opt.brand = detected;
        }
    }

    const int eff_len = cards::effective_length(opt);
    int eff_cvv = cards::effective_cvv_length(opt);

    // If --cvv was supplied without --cvv-length, auto-size CVV length to match.
    if (!opt.fixed_cvv.empty() && opt.cvv_length_override == 0 &&
        static_cast<int>(opt.fixed_cvv.size()) != eff_cvv) {
        opt.cvv_length_override = static_cast<int>(opt.fixed_cvv.size());
        eff_cvv = opt.cvv_length_override;
    }

    if (!opt.custom_prefix.empty() && static_cast<int>(opt.custom_prefix.size()) >= eff_len) {
        std::fprintf(stderr,
            "generator: --bin (%zu digits) is not shorter than card length (%d)\n",
            opt.custom_prefix.size(), eff_len);
        return 2;
    }
    if (!opt.fixed_cvv.empty() && static_cast<int>(opt.fixed_cvv.size()) != eff_cvv) {
        std::fprintf(stderr,
            "generator: --cvv has %zu digits but CVV length is %d\n",
            opt.fixed_cvv.size(), eff_cvv);
        return 2;
    }
    if (opt.year_min >= 0 && opt.year_max >= 0 && opt.year_max < opt.year_min) {
        std::fprintf(stderr, "generator: --year-max (%d) is less than --year-min (%d)\n",
                     opt.year_max, opt.year_min);
        return 2;
    }

    // Reject expired fixed expiry unless explicitly allowed.
    if (!allow_expired) {
        const cards::Date today = cards::current_date();
        auto in_past_yy = [&](int yy) {
            const int full = cards::resolve_full_year(yy, today.year);
            return full < today.year;
        };
        if (opt.fixed_year >= 0 && opt.fixed_month > 0) {
            const int full = cards::resolve_full_year(opt.fixed_year, today.year);
            if (full < today.year || (full == today.year && opt.fixed_month < today.month)) {
                std::fprintf(stderr,
                    "generator: --expiry %02d/%02d is in the past. Use --allow-expired if intentional.\n",
                    opt.fixed_month, opt.fixed_year);
                return 2;
            }
        } else if (opt.fixed_year >= 0 && in_past_yy(opt.fixed_year)) {
            std::fprintf(stderr,
                "generator: --year %02d is in the past. Use --allow-expired if intentional.\n",
                opt.fixed_year);
            return 2;
        }
        if (opt.year_max >= 0 && in_past_yy(opt.year_max)) {
            std::fprintf(stderr,
                "generator: --year-max %02d is in the past. Use --allow-expired if intentional.\n",
                opt.year_max);
            return 2;
        }
    }

    // Optional BIN database for --info. We load it lazily so users who do
    // not need it don't pay the ~150 ms startup cost.
    cards::BinDatabase bin_db;
    bool bin_db_ready = false;
    if (with_info) {
        bin_db_ready = ensure_bin_db(bin_db, bin_db_path, /*warn_if_missing=*/true);
        if (!bin_db_ready) return 2;
    }

    static char stdout_buf[1 << 20]; // 1 MiB
    std::setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));

    if (!with_info) {
        cards::generate(count, opt, [](const char* p, std::size_t n) {
            std::fwrite(p, 1, n, stdout);
        });
        return 0;
    }

    // --info: extend each line with BIN-database fields. We need to look at
    // the generated text after generate() emits it, so we use a small line
    // buffer and walk the buffer line-by-line. This keeps the hot path inside
    // generate() unchanged.
    if (opt.header && opt.output != cards::OutputFormat::Json) {
        // Replace the default header with one that includes the extra cols.
        std::string h = "number"; h += opt.separator;
        h += "expiry"; h += opt.separator;
        h += "cvv";    h += opt.separator;
        h += "bank";   h += opt.separator;
        h += "type";   h += opt.separator;
        h += "country";
        if (opt.output == cards::OutputFormat::Csv) {
            h += opt.separator; h += "category";
        }
        h += "\n";
        std::fwrite(h.data(), 1, h.size(), stdout);
        opt.header = false;
    }

    std::string pending;
    pending.reserve(8 * 1024);
    std::string suffix;
    suffix.reserve(256);

    cards::generate(count, opt, [&](const char* p, std::size_t n) {
        pending.append(p, n);
        size_t start = 0;
        while (true) {
            const size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;

            std::string_view line(pending.data() + start, nl - start);
            // Carve out the BIN: skip optional leading whitespace, then read
            // the first up-to-6 digits.
            std::string bin;
            bin.reserve(6);
            const bool is_json = (opt.output == cards::OutputFormat::Json);
            const char* scan = line.data();
            const char* end  = scan + line.size();
            if (is_json) {
                static constexpr char K[] = "\"number\":\"";
                const char* k = std::search(scan, end, K, K + sizeof(K) - 1);
                if (k != end) scan = k + sizeof(K) - 1;
            }
            for (; scan < end && bin.size() < 6; ++scan) {
                char c = *scan;
                if (c >= '0' && c <= '9') bin.push_back(c);
                else if (c == ' ' || c == '-' || c == '"') continue;
                else break;
            }

            const cards::BinInfo* info = nullptr;
            if (bin.size() == 6) info = bin_db.lookup(bin);

            suffix.clear();
            if (is_json) {
                // Insert the suffix just before the closing brace.
                size_t close = line.find_last_of('}');
                if (close == std::string_view::npos) {
                    // Shouldn't happen with our generator, but fall back to
                    // appending and let downstream handle it.
                    std::fwrite(line.data(), 1, line.size(), stdout);
                    suffix = format_bin_info(info, opt.separator, opt.output);
                    std::fwrite(suffix.data(), 1, suffix.size(), stdout);
                    std::fputc('\n', stdout);
                } else {
                    std::fwrite(line.data(), 1, close, stdout);
                    suffix = format_bin_info(info, opt.separator, opt.output);
                    std::fwrite(suffix.data(), 1, suffix.size(), stdout);
                    std::fputc('}', stdout);
                    std::fputc('\n', stdout);
                }
            } else {
                std::fwrite(line.data(), 1, line.size(), stdout);
                suffix = format_bin_info(info, opt.separator, opt.output);
                std::fwrite(suffix.data(), 1, suffix.size(), stdout);
                std::fputc('\n', stdout);
            }
            start = nl + 1;
        }
        if (start > 0) pending.erase(0, start);
    });

    if (!pending.empty()) {
        // Should be empty, but flush it so we never lose data.
        std::fwrite(pending.data(), 1, pending.size(), stdout);
    }
    return 0;
}
