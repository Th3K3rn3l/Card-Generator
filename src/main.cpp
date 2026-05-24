#include "cards.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

static void print_usage(std::FILE* out) {
    std::fprintf(out,
        "Usage: generator <count> [options]\n"
        "\n"
        "Generates <count> Luhn-valid synthetic card records.\n"
        "Default line: <card-number> <MM/YY> <CVV>\n"
        "\n"
        "Brand & prefix:\n"
        "  -b, --brand <name>     visa | mc | amex | discover | jcb (default: visa)\n"
        "      --bin <digits>     Custom BIN/IIN prefix. Brand is auto-detected\n"
        "                         from this BIN unless --brand is set explicitly.\n"
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
        "\n"
        "Output:\n"
        "      --no-format        Do not insert spaces between groups of 4 digits.\n"
        "      --sep <s>          Field separator (default: \" \").\n"
        "      --json             JSON Lines: {\"number\":...,\"expiry\":...,\"cvv\":...}.\n"
        "      --csv              Shortcut: --sep ',' --no-format --header.\n"
        "      --header           Emit a 'number<sep>expiry<sep>cvv' header line first.\n"
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

int main(int argc, char** argv) {
    long long count = 0;
    cards::Options opt;
    opt.brand = cards::find_brand("visa");
    bool brand_explicit = false;

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

    if (!opt.custom_prefix.empty() && !brand_explicit) {
        if (const auto* d = cards::detect_brand_from_bin(opt.custom_prefix)) opt.brand = d;
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

    static char stdout_buf[1 << 20]; // 1 MiB
    std::setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));

    cards::generate(count, opt, [](const char* p, std::size_t n) {
        std::fwrite(p, 1, n, stdout);
    });
    return 0;
}
