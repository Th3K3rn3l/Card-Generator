#include "cards.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

namespace {

struct Collector {
    std::string buf;
    void operator()(const char* p, std::size_t n) { buf.append(p, n); }
};

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::vector<std::string> split_by(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(c, i);
        if (j == std::string::npos) j = s.size();
        out.emplace_back(s.substr(i, j - i));
        if (j == s.size()) break;
        i = j + 1;
    }
    return out;
}

int failures = 0;
template <class F>
void test(const char* name, F&& f) {
    try {
        f();
        std::printf("ok   %s\n", name);
    } catch (const std::exception& e) {
        failures++;
        std::printf("FAIL %s\n     %s\n", name, e.what());
    } catch (...) {
        failures++;
        std::printf("FAIL %s\n     (unknown exception)\n", name);
    }
}

#define REQUIRE(cond, msg) do { if (!(cond)) throw std::runtime_error(std::string(msg) + " :: " #cond); } while (0)

} // namespace

int main() {
    using namespace cards;

    test("luhn_check basic vectors", []{
        REQUIRE(luhn_check("4539578763621486"), "known good visa");
        REQUIRE(luhn_check("79927398713"),       "wiki classic");
        REQUIRE(!luhn_check("79927398710"),      "wrong checksum");
        REQUIRE(!luhn_check("4539578763621487"), "tampered visa");
        REQUIRE(!luhn_check(""),                 "empty");
        REQUIRE(!luhn_check("12a4"),             "non-digit");
    });

    test("generates exactly N records", []{
        Options opt; opt.brand = find_brand("visa");
        Collector c;
        generate(1000, opt, std::ref(c));
        REQUIRE(split_lines(c.buf).size() == 1000, "line count");
    });

    test("visa: 16 digits, prefix 4, luhn valid", []{
        Options opt; opt.brand = find_brand("visa");
        Collector c;
        generate(300, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            REQUIRE(parts.size() >= 3, "fields");
            std::string num = parts[0];
            for (size_t k = 1; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 16, "visa length");
            REQUIRE(num[0] == '4', "visa prefix");
            REQUIRE(luhn_check(num), "visa luhn");
            const std::string& exp = parts[parts.size() - 2];
            REQUIRE(exp.size() == 5 && exp[2] == '/', "expiry format");
        }
    });

    test("amex: 15 digits, 4-digit cvv", []{
        Options opt; opt.brand = find_brand("amex");
        Collector c;
        generate(200, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            const std::string& cvv = parts.back();
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 15, "amex length");
            REQUIRE(luhn_check(num), "amex luhn");
            REQUIRE(cvv.size() == 4, "amex cvv");
        }
    });

    test("--no-format three fields", []{
        Options opt; opt.brand = find_brand("mc"); opt.format = false;
        Collector c;
        generate(100, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            REQUIRE(parts.size() == 3, "three fields");
            REQUIRE(parts[0].size() == 16, "mc length");
            REQUIRE(luhn_check(parts[0]), "mc luhn");
        }
    });

    test("custom BIN: every card starts with that BIN", []{
        Options opt; opt.brand = find_brand("visa"); opt.custom_prefix = "414720";
        Collector c;
        generate(200, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            std::string full;
            for (size_t k = 0; k + 2 < parts.size(); k++) full += parts[k];
            REQUIRE(full.compare(0, 6, "414720") == 0, "BIN prefix");
            REQUIRE(luhn_check(full), "luhn");
        }
    });

    test("detect_brand_from_bin matches longest prefix", []{
        REQUIRE(detect_brand_from_bin("411111")->name == "visa",     "visa");
        REQUIRE(detect_brand_from_bin("552233")->name == "mc",       "mc");
        REQUIRE(detect_brand_from_bin("371449")->name == "amex",     "amex");
        REQUIRE(detect_brand_from_bin("601138")->name == "discover", "discover");
        REQUIRE(detect_brand_from_bin("352800")->name == "jcb",      "jcb");
        REQUIRE(detect_brand_from_bin("000000") == nullptr,          "unknown");
    });

    test("--expiry fixes both month and year", []{
        Options opt; opt.brand = find_brand("visa");
        opt.fixed_month = 12; opt.fixed_year = 30;
        Collector c; generate(100, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            const std::string& exp = parts[parts.size() - 2];
            REQUIRE(exp == "12/30", "fixed expiry");
        }
    });

    test("--month fixes month, year still varies", []{
        Options opt; opt.brand = find_brand("visa"); opt.fixed_month = 7;
        Collector c; generate(200, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            const std::string& exp = parts[parts.size() - 2];
            REQUIRE(exp.substr(0, 2) == "07", std::string("fixed month: ") + exp);
        }
    });

    test("--year fixes year, month still varies", []{
        Options opt; opt.brand = find_brand("visa"); opt.fixed_year = 33;
        Collector c; generate(200, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            const std::string& exp = parts[parts.size() - 2];
            REQUIRE(exp.substr(3, 2) == "33", std::string("fixed year: ") + exp);
        }
    });

    test("--cvv: every CVV equals supplied value", []{
        Options opt; opt.brand = find_brand("visa"); opt.fixed_cvv = "123";
        Collector c; generate(100, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            REQUIRE(parts.back() == "123", "fixed cvv");
        }
    });

    test("--length and --cvv-length overrides", []{
        Options opt; opt.brand = find_brand("visa");
        opt.length_override = 19; opt.cvv_length_override = 4;
        Collector c; generate(50, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 19, std::string("length: ") + std::to_string(num.size()));
            REQUIRE(luhn_check(num), "luhn");
            REQUIRE(parts.back().size() == 4, "cvv length");
        }
    });

    test("--no-luhn produces invalid Luhn checksum sometimes", []{
        Options opt; opt.brand = find_brand("visa"); opt.enforce_luhn = false;
        Collector c; generate(500, opt, std::ref(c));
        int bad = 0;
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            if (!luhn_check(num)) bad++;
        }
        REQUIRE(bad > 100, std::string("expected many non-luhn, got: ") + std::to_string(bad));
    });

    test("--seed makes output deterministic", []{
        Options opt; opt.brand = find_brand("visa"); opt.seed_set = true; opt.seed = 42;
        Collector a, b;
        generate(50, opt, std::ref(a));
        generate(50, opt, std::ref(b));
        REQUIRE(a.buf == b.buf, "deterministic output");
    });

    test("different seeds → different output", []{
        Options o1; o1.brand = find_brand("visa"); o1.seed_set = true; o1.seed = 1;
        Options o2 = o1; o2.seed = 2;
        Collector a, b;
        generate(50, o1, std::ref(a));
        generate(50, o2, std::ref(b));
        REQUIRE(a.buf != b.buf, "different seeds give different output");
    });

    test("JSON output: well-formed and valid", []{
        Options opt; opt.brand = find_brand("visa"); opt.output = OutputFormat::Json;
        opt.seed_set = true; opt.seed = 7;
        Collector c; generate(20, opt, std::ref(c));
        auto lines = split_lines(c.buf);
        REQUIRE(lines.size() == 20, "20 json lines");
        for (const auto& ln : lines) {
            REQUIRE(ln.front() == '{' && ln.back() == '}', "json braces");
            REQUIRE(ln.find("\"number\":\"") != std::string::npos, "number key");
            REQUIRE(ln.find("\",\"expiry\":\"") != std::string::npos, "expiry key");
            REQUIRE(ln.find("\",\"cvv\":\"") != std::string::npos, "cvv key");
        }
    });

    test("CSV output: header + comma-separated rows", []{
        Options opt; opt.brand = find_brand("visa");
        opt.output = OutputFormat::Csv;
        opt.separator = ","; opt.format = false; opt.header = true;
        Collector c; generate(10, opt, std::ref(c));
        auto lines = split_lines(c.buf);
        REQUIRE(lines.size() == 11, "10 rows + header");
        REQUIRE(lines[0] == "number,expiry,cvv", std::string("header line: ") + lines[0]);
        for (size_t i = 1; i < lines.size(); i++) {
            auto parts = split_by(lines[i], ',');
            REQUIRE(parts.size() == 3, "3 columns");
            REQUIRE(parts[0].size() == 16, "no-format visa length");
            REQUIRE(luhn_check(parts[0]), "luhn");
        }
    });

    test("year-min/max bounds respected", []{
        Options opt; opt.brand = find_brand("visa");
        opt.year_min = 40; opt.year_max = 42;
        Collector c; generate(300, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            const std::string& exp = parts[parts.size() - 2];
            int y = (exp[3] - '0') * 10 + (exp[4] - '0');
            REQUIRE(y >= 40 && y <= 42, std::string("year out of range: ") + exp);
        }
    });

    test("all brands produce non-empty output", []{
        for (const auto& b : brands()) {
            Options opt; opt.brand = &b;
            Collector c; generate(20, opt, std::ref(c));
            REQUIRE(!c.buf.empty(), std::string("brand: ") + std::string(b.name));
        }
    });

    test("all_digits validation", []{
        REQUIRE(all_digits("414720"), "digits");
        REQUIRE(!all_digits(""),      "empty");
        REQUIRE(!all_digits("12a4"),  "letter");
        REQUIRE(!all_digits("12 4"),  "space");
    });

    test("throughput sanity: >= 1M cards/sec", []{
        Options opt; opt.brand = find_brand("visa");
        auto t0 = std::chrono::high_resolution_clock::now();
        long long bytes = 0;
        generate(200'000, opt, [&](const char*, std::size_t n){ bytes += (long long)n; });
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double per_sec = 200'000.0 / (ms / 1000.0);
        std::printf("     (%.0f cards/s, %lld bytes)\n", per_sec, bytes);
        REQUIRE(per_sec >= 1'000'000.0, "throughput too low");
    });

    if (failures) {
        std::printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
