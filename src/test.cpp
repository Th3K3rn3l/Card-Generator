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

std::vector<std::string> split_spaces(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.emplace_back(s.substr(i, j - i));
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
        auto lines = split_lines(c.buf);
        REQUIRE(lines.size() == 1000, "line count");
    });

    test("visa: 16 digits, prefix 4, luhn valid", []{
        Options opt; opt.brand = find_brand("visa");
        Collector c;
        generate(500, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_spaces(ln);
            REQUIRE(parts.size() >= 3, "fields");
            const std::string& cvv = parts.back();
            const std::string& exp = parts[parts.size() - 2];
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 16, "visa length");
            REQUIRE(num[0] == '4', "visa prefix");
            REQUIRE(luhn_check(num), "visa luhn");
            REQUIRE(exp.size() == 5 && exp[2] == '/', "expiry format");
            int mm = (exp[0]-'0')*10 + (exp[1]-'0');
            REQUIRE(mm >= 1 && mm <= 12, "month range");
            REQUIRE(cvv.size() == 3, "cvv length");
        }
    });

    test("amex: 15 digits, 4-digit cvv, prefix 34/37", []{
        Options opt; opt.brand = find_brand("amex");
        Collector c;
        generate(300, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_spaces(ln);
            const std::string& cvv = parts.back();
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 15, "amex length");
            REQUIRE(num[0] == '3' && (num[1] == '4' || num[1] == '7'), "amex prefix");
            REQUIRE(luhn_check(num), "amex luhn");
            REQUIRE(cvv.size() == 4, "amex cvv");
        }
    });

    test("--no-format produces 3 fields", []{
        Options opt; opt.brand = find_brand("mc"); opt.format = false;
        Collector c;
        generate(100, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_spaces(ln);
            REQUIRE(parts.size() == 3, "three fields");
            REQUIRE(parts[0].size() == 16, "mc length");
            REQUIRE(luhn_check(parts[0]), "mc luhn");
        }
    });

    test("all brands produce non-empty output", []{
        for (const auto& b : brands()) {
            Options opt; opt.brand = &b;
            Collector c;
            generate(20, opt, std::ref(c));
            REQUIRE(!c.buf.empty(), std::string("brand: ") + std::string(b.name));
        }
    });

    test("custom BIN: every card starts with that BIN, still Luhn-valid", []{
        Options opt; opt.brand = find_brand("visa"); opt.custom_prefix = "414720";
        Collector c;
        generate(300, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_spaces(ln);
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 16, "visa length");
            REQUIRE(num.compare(0, 6, "414720") == 0, std::string("BIN prefix mismatch: ") + num);
            REQUIRE(luhn_check(num), std::string("luhn fail: ") + num);
        }
    });

    test("custom BIN works with amex length/cvv", []{
        Options opt; opt.brand = find_brand("amex"); opt.custom_prefix = "377777";
        Collector c;
        generate(200, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_spaces(ln);
            const std::string& cvv = parts.back();
            std::string num;
            for (size_t k = 0; k + 2 < parts.size(); k++) num += parts[k];
            REQUIRE(num.size() == 15, "amex+BIN length");
            REQUIRE(num.compare(0, 6, "377777") == 0, "amex BIN prefix");
            REQUIRE(cvv.size() == 4, "amex+BIN cvv");
            REQUIRE(luhn_check(num), "amex+BIN luhn");
        }
    });

    test("detect_brand_from_bin picks the longest matching prefix", []{
        REQUIRE(detect_brand_from_bin("411111")->name == "visa", "visa BIN");
        REQUIRE(detect_brand_from_bin("552233")->name == "mc",   "mc BIN");
        REQUIRE(detect_brand_from_bin("371449")->name == "amex", "amex BIN");
        REQUIRE(detect_brand_from_bin("601138")->name == "discover", "discover BIN");
        REQUIRE(detect_brand_from_bin("352800")->name == "jcb",  "jcb BIN");
        REQUIRE(detect_brand_from_bin("000000") == nullptr,      "unknown BIN");
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
