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
        "Each line: <card-number> <MM/YY> <CVV>\n"
        "\n"
        "Options:\n"
        "  -b, --brand <name>   visa | mc | amex | discover | jcb (default: visa)\n"
        "      --no-format      Do not insert spaces between groups of 4 digits\n"
        "      --sep <s>        Field separator (default: single space)\n"
        "      --year-span <n>  Years of validity range above current (default: 6)\n"
        "  -h, --help           Show this help\n"
        "\n"
        "Examples:\n"
        "  generator 100\n"
        "  generator 1000 --brand mc\n"
        "  generator 50000 --brand amex --no-format > cards.txt\n");
}

int main(int argc, char** argv) {
    long long count = 0;
    cards::Options opt;
    opt.brand = cards::find_brand("visa");

    for (int i = 1; i < argc; i++) {
        std::string_view a(argv[i]);
        if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else if (a == "-b" || a == "--brand") {
            if (++i >= argc) { std::fprintf(stderr, "generator: missing brand name\n"); return 2; }
            const auto* b = cards::find_brand(argv[i]);
            if (!b) { std::fprintf(stderr, "generator: unknown brand: %s\n", argv[i]); return 2; }
            opt.brand = b;
        } else if (a == "--no-format") {
            opt.format = false;
        } else if (a == "--sep") {
            if (++i >= argc) { std::fprintf(stderr, "generator: missing separator\n"); return 2; }
            opt.separator = argv[i];
        } else if (a == "--year-span") {
            if (++i >= argc) { std::fprintf(stderr, "generator: missing year span\n"); return 2; }
            int v = std::atoi(argv[i]);
            if (v < 1) v = 1;
            opt.year_span = v;
        } else if (count == 0) {
            char* end = nullptr;
            long long v = std::strtoll(argv[i], &end, 10);
            if (!end || *end != '\0' || v <= 0) {
                std::fprintf(stderr, "generator: invalid count: %s\n", argv[i]);
                return 2;
            }
            count = v;
        } else {
            std::fprintf(stderr, "generator: unknown argument: %s\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }

    if (count == 0) {
        print_usage(stderr);
        return 1;
    }

    static char stdout_buf[1 << 20]; // 1 MiB
    std::setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));

    cards::generate(count, opt, [](const char* p, std::size_t n) {
        std::fwrite(p, 1, n, stdout);
    });
    return 0;
}
