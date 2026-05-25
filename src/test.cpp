#include "bin_db.hpp"
#include "cards.hpp"
#include "scheme.hpp"
#include "validator.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
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

    // ---- normalize_number / validate_* (negative tests) ---------------

    test("normalize_number strips spaces, dashes, underscores, tabs", []{
        auto a = normalize_number("4929 9231 4271 9262");
        REQUIRE(a.ok && a.digits == "4929923142719262", "spaces");
        auto b = normalize_number("4929-9231-4271-9262");
        REQUIRE(b.ok && b.digits == "4929923142719262", "dashes");
        auto c = normalize_number("4929_9231\t4271 9262");
        REQUIRE(c.ok && c.digits == "4929923142719262", "mixed");
    });

    test("normalize_number rejects letters and punctuation", []{
        REQUIRE(!normalize_number("4929A231").ok, "letter");
        REQUIRE(!normalize_number("4929.9231").ok, "dot");
        REQUIRE(!normalize_number("4929/9231").ok, "slash");
    });

    test("validate_number: happy path", []{
        auto r = validate_number("4929 9231 4271 9262");
        REQUIRE(r.ok(), r.what());
        REQUIRE(r.brand && r.brand->name == "visa", "brand detected");
        REQUIRE(r.normalized == "4929923142719262", "normalized");
    });

    test("validate_number: empty after normalization", []{
        REQUIRE(validate_number("   ").err == ValidationError::Empty, "empty");
    });

    test("validate_number: tampered Luhn caught", []{
        auto r = validate_number("4929 9231 4271 9263");
        REQUIRE(r.err == ValidationError::BadLuhn, r.what());
    });

    test("validate_number: bad length for detected brand", []{
        // 16 starting with 4 is visa; trim one digit and we should get BadLength
        // (the new BIN still detects as visa).
        auto r = validate_number("492992314271926");
        REQUIRE(r.err == ValidationError::BadLength, r.what());
    });

    test("validate_number: unknown brand from BIN", []{
        // 13-digit number that doesn't match any known prefix.
        auto r = validate_number("0000000000000");
        REQUIRE(r.err == ValidationError::UnknownBrand, r.what());
    });

    test("validate_number: brand mismatch when expected supplied", []{
        auto r = validate_number("4929923142719262", find_brand("mc"));
        REQUIRE(r.err == ValidationError::BrandMismatch, r.what());
    });

    test("validate_card: bad month", []{
        auto r = validate_card("4929923142719262", 13, 30, "123");
        REQUIRE(r.err == ValidationError::BadMonth, r.what());
        auto r2 = validate_card("4929923142719262", 0, 30, "123");
        REQUIRE(r2.err == ValidationError::BadMonth, r2.what());
    });

    test("validate_card: expired vs future", []{
        // Today fixed at 2026-05 to keep the test stable.
        Date today{2026, 5};
        auto past = validate_card("4929923142719262", 4, 26, "123", nullptr, today);
        REQUIRE(past.err == ValidationError::Expired, past.what());
        auto same_month = validate_card("4929923142719262", 5, 26, "123", nullptr, today);
        REQUIRE(same_month.ok(), same_month.what());
        auto future = validate_card("4929923142719262", 12, 30, "123", nullptr, today);
        REQUIRE(future.ok(), future.what());
    });

    test("validate_card: cvv length and digits", []{
        auto bad_len = validate_card("4929923142719262", 12, 30, "12");
        REQUIRE(bad_len.err == ValidationError::BadCvvLength, bad_len.what());
        auto non_digit = validate_card("4929923142719262", 12, 30, "12a");
        REQUIRE(non_digit.err == ValidationError::BadCvvDigits, non_digit.what());
        // Amex needs 4-digit CVV.
        auto amex_bad = validate_card("371449635398431", 12, 30, "123", find_brand("amex"));
        REQUIRE(amex_bad.err == ValidationError::BadCvvLength, amex_bad.what());
    });

    test("validate_card_str: parses MM/YY", []{
        Date today{2026, 5};
        auto good = validate_card_str("4929923142719262", "12/30", "123", nullptr, today);
        REQUIRE(good.ok(), good.what());
        auto bad = validate_card_str("4929923142719262", "13/30", "123", nullptr, today);
        REQUIRE(bad.err == ValidationError::BadMonth, bad.what());
        auto malformed = validate_card_str("4929923142719262", "12-30", "123", nullptr, today);
        REQUIRE(malformed.err == ValidationError::BadMonth, malformed.what());
    });

    test("resolve_full_year: same century, then rolls over", []{
        REQUIRE(resolve_full_year(26, 2026) == 2026, "same year");
        REQUIRE(resolve_full_year(99, 2026) == 2099, "future");
        // Far past (>50 years behind today) rolls into the next century.
        REQUIRE(resolve_full_year(0, 2080) == 2100, "rollover");
    });

    test("brand_matches_prefix: exact and longest", []{
        REQUIRE(brand_matches_prefix(find_brand("visa"), "4111111111111111"), "visa 4");
        REQUIRE(!brand_matches_prefix(find_brand("visa"), "5111"),            "visa not 5");
        REQUIRE(brand_matches_prefix(find_brand("amex"), "371449"),           "amex 37");
        REQUIRE(!brand_matches_prefix(find_brand("amex"), "351449"),          "amex not 35");
    });

    test("generated cards pass validate_card", []{
        Options opt; opt.brand = find_brand("mc");
        opt.fixed_month = 6; opt.fixed_year = 30;
        opt.fixed_cvv = "123";
        opt.format = false;
        Collector c; generate(200, opt, std::ref(c));
        Date today{2026, 5};
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            REQUIRE(parts.size() == 3, "fields");
            auto r = validate_card_str(parts[0], parts[1], parts[2], opt.brand, today);
            REQUIRE(r.ok(), std::string("validate failed: ") + r.what());
        }
    });

    // ---- BinDatabase ---------------------------------------------------

    test("BinDatabase: parses CSV with embedded quotes and commas", []{
        const char* csv =
            "BIN,Brand,Type,Category,Issuer,IssuerPhone,IssuerUrl,isoCode2,isoCode3,CountryName\n"
            "411111,VISA,CREDIT,CLASSIC,\"BANK OF \"\"X\"\", LTD\",,https://x.example,US,USA,\"UNITED STATES\"\n"
            "414720,VISA,CREDIT,TRADITIONAL,JPMORGAN,\"+1 800 555 0000\",https://chase.com,US,USA,\"UNITED STATES\"\n"
            "555555,MASTERCARD,DEBIT,STANDARD,SOMEBANK,,,GB,GBR,\"UNITED KINGDOM\"\n"
            "002101,\"PRIVATE LABEL\",CHARGE CARD,,,,,CN,CHN,CHINA\n";
        BinDatabase db;
        REQUIRE(db.load_from_memory(csv), "load");
        REQUIRE(db.size() == 4, std::string("size: ") + std::to_string(db.size()));

        auto* a = db.lookup("411111");
        REQUIRE(a != nullptr, "411111");
        REQUIRE(a->type == CardType::Credit, "credit");
        REQUIRE(a->brand == "VISA", "brand");
        REQUIRE(a->issuer == "BANK OF \"X\", LTD", std::string("escaped: ") + std::string(a->issuer));
        REQUIRE(a->country == "UNITED STATES", "country");

        auto* b = db.lookup("414720");
        REQUIRE(b && b->issuer == "JPMORGAN", "jpmorgan");
        REQUIRE(b->issuer_phone == "+1 800 555 0000", "phone");

        auto* c = db.lookup("555555");
        REQUIRE(c && c->type == CardType::Debit, "debit");

        auto* d = db.lookup("002101");
        REQUIRE(d && d->type == CardType::Charge, "charge");

        REQUIRE(db.lookup("999999") == nullptr, "miss");
    });

    test("BinDatabase: short prefix returns longest matching record", []{
        const char* csv =
            "BIN,Brand,Type,Category,Issuer,IssuerPhone,IssuerUrl,isoCode2,isoCode3,CountryName\n"
            "414720,VISA,CREDIT,,JPMORGAN,,,US,USA,USA\n"
            "414730,VISA,CREDIT,,CHASE2,,,US,USA,USA\n";
        BinDatabase db;
        REQUIRE(db.load_from_memory(csv), "load");
        // "4147" should pick the first record with bin in [414700, 414799],
        // which is 414720 (lower numeric value).
        auto* a = db.lookup("4147");
        REQUIRE(a && a->bin == 414720, "prefix lookup");
        auto* b = db.lookup("41473");
        REQUIRE(b && b->bin == 414730, "5-digit prefix");
        auto* miss = db.lookup("4148");
        REQUIRE(miss == nullptr, "no match");
    });

    test("BinDatabase: rejects bad input", []{
        const char* csv =
            "BIN,Brand,Type,Category,Issuer,IssuerPhone,IssuerUrl,isoCode2,isoCode3,CountryName\n"
            "411111,VISA,CREDIT,,X,,,US,USA,USA\n";
        BinDatabase db;
        db.load_from_memory(csv);
        REQUIRE(db.lookup("") == nullptr, "empty");
        REQUIRE(db.lookup("41a111") == nullptr, "letter");
        REQUIRE(db.lookup("4111111") == nullptr, "too long");
    });

    test("BinDatabase: real upstream file (if present)", []{
        std::string path = default_bin_db_path();
        if (path.empty()) {
            std::printf("     (skipped: no BIN database on disk)\n");
            return;
        }
        BinDatabase db;
        REQUIRE(db.load(path), std::string("load: ") + path);
        REQUIRE(db.size() > 100000, std::string("size: ") + std::to_string(db.size()));
        // 414720 = JPMorgan Chase (well-known stable test BIN).
        auto* info = db.lookup("414720");
        REQUIRE(info != nullptr, "414720 not found");
        REQUIRE(info->iso_alpha2 == "US", std::string("iso2: ") + std::string(info->iso_alpha2));
    });

    // ---- Scheme detection (extended) -----------------------------------

    test("scheme: visa accepts 13/16/19", []{
        const Scheme* s = find_scheme("visa");
        REQUIRE(s, "visa");
        REQUIRE(s->accepts_length(13), "13");
        REQUIRE(s->accepts_length(16), "16");
        REQUIRE(s->accepts_length(19), "19");
        REQUIRE(!s->accepts_length(15), "not 15");
    });

    test("scheme: mastercard 2-series detected", []{
        // 2221-2720 is mainstream Mastercard since 2017.
        REQUIRE(detect_scheme("2221001234567890")->name == "mastercard", "2221");
        REQUIRE(detect_scheme("2720001234567890")->name == "mastercard", "2720");
        REQUIRE(detect_scheme("2730001234567890") == nullptr ||
                detect_scheme("2730001234567890")->name != "mastercard", "2730 out of range");
    });

    test("scheme: longest-prefix wins between Discover and UnionPay", []{
        // 622126-622925 is Discover; 62 generally is UnionPay.
        REQUIRE(detect_scheme("6221261234567890")->name == "discover", "Discover wins");
        REQUIRE(detect_scheme("6299991234567890")->name == "unionpay", "UnionPay default");
    });

    test("scheme: mir / rupay / verve recognized", []{
        REQUIRE(detect_scheme("2200123412341234")->name == "mir",   "mir");
        REQUIRE(detect_scheme("6521012341234567")->name == "rupay", "rupay");
        REQUIRE(detect_scheme("5060991234567890")->name == "verve", "verve");
    });

    test("scheme: amex 15 / cvv 4", []{
        const Scheme* s = find_scheme("amex");
        REQUIRE(s->accepts_length(15) && !s->accepts_length(16), "amex 15");
        REQUIRE(s->accepts_cvv_length(4) && !s->accepts_cvv_length(3), "amex 4-cvv");
    });

    // ---- validate_full ----------------------------------------------------

    test("validate_full: clean Visa is fully clean", []{
        ValidateInput in;
        in.number = "4929 9231 4271 9262";
        in.expiry = "12/30";
        in.cvv    = "123";
        in.today  = Date{2026, 5};
        Report r = validate_full(in);
        REQUIRE(r.ok(), "ok");
        REQUIRE(r.scheme && r.scheme->name == "visa", "visa");
        REQUIRE(r.luhn_ok, "luhn");
        REQUIRE(r.normalized_number == "4929923142719262", "normalized");
        // Expect at least one Info note for the stripped spaces, plus the
        // CVK-not-checked reminder.
        bool seen_strip = false, seen_cvk = false;
        for (const auto& i : r.issues) {
            if (i.code == IssueCode::NormalizedSeparators) seen_strip = true;
            if (i.code == IssueCode::CvkNotChecked)        seen_cvk   = true;
        }
        REQUIRE(seen_strip, "strip note");
        REQUIRE(seen_cvk,   "cvk note");
    });

    test("validate_full: Stripe test card flagged as known", []{
        ValidateInput in;
        in.number = "4242424242424242";
        in.today = Date{2026, 5};
        Report r = validate_full(in);
        REQUIRE(r.ok(), "still passes structural checks");
        bool seen = false;
        for (const auto& i : r.issues) if (i.code == IssueCode::KnownTestCard) seen = true;
        REQUIRE(seen, "known test card warning");
    });

    test("validate_full: low-entropy and sequential warnings", []{
        // 4111 1111 1111 1111 is structurally valid Visa and a known test PAN.
        ValidateInput in;
        in.number = "4111111111111111";
        in.today  = Date{2026, 5};
        Report r = validate_full(in);
        bool seen_low = false;
        for (const auto& i : r.issues)
            if (i.code == IssueCode::LowEntropy || i.code == IssueCode::AllSameDigit)
                seen_low = true;
        REQUIRE(seen_low, "low-entropy detected");

        // 1234 5678 9012 3452 has a long sequential run.
        ValidateInput in2;
        in2.number = "1234567890123452";  // luhn-valid, mostly sequential
        in2.today = Date{2026, 5};
        Report r2 = validate_full(in2);
        bool seen_seq = false;
        for (const auto& i : r2.issues) if (i.code == IssueCode::SequentialDigits) seen_seq = true;
        REQUIRE(seen_seq, "sequential warning");
    });

    test("validate_full: unicode digits and separators", []{
        // U+FF14 = full-width '4', U+FF12 = full-width '2', etc.
        // We pass 4242424242424242 with the first '4' as full-width.
        std::string num = "\xEF\xBC\x94" "242424242424242";  // FW4 + ASCII rest
        ValidateInput in;
        in.number = num;
        in.today  = Date{2026, 5};
        Report r = validate_full(in);
        REQUIRE(r.normalized_number == "4242424242424242", r.normalized_number);
        bool seen = false;
        for (const auto& i : r.issues) if (i.code == IssueCode::NonAsciiInputAccepted) seen = true;
        REQUIRE(seen, "non-ascii note");
    });

    test("validate_full: NBSP and en-dash separators are stripped", []{
        std::string num = "4242\xC2\xA0" "4242\xE2\x80\x93" "4242 4242";
        ValidateInput in;
        in.number = num;
        in.today  = Date{2026, 5};
        Report r = validate_full(in);
        REQUIRE(r.normalized_number == "4242424242424242", r.normalized_number);
    });

    test("validate_full: BadLuhn is reported as error", []{
        ValidateInput in;
        in.number = "4242424242424241";
        in.today = Date{2026, 5};
        Report r = validate_full(in);
        REQUIRE(!r.ok(), "fails");
        bool seen = false;
        for (const auto& i : r.issues)
            if (i.code == IssueCode::BadLuhn && i.severity == Severity::Error) seen = true;
        REQUIRE(seen, "bad luhn error");
    });

    test("validate_full: BadLength reports accepted set", []{
        ValidateInput in;
        in.number = "424242424242424"; // 15 digits, Visa expects 13/16/19
        in.today = Date{2026, 5};
        Report r = validate_full(in);
        bool seen = false;
        for (const auto& i : r.issues)
            if (i.code == IssueCode::BadLength) seen = true;
        REQUIRE(seen, "bad length");
    });

    test("validate_full: cvv length per scheme", []{
        ValidateInput in_visa;
        in_visa.number = "4929923142719262"; in_visa.cvv = "1234";
        in_visa.today = Date{2026, 5};
        bool visa_bad = false;
        for (const auto& i : validate_full(in_visa).issues)
            if (i.code == IssueCode::BadCvvLength) visa_bad = true;
        REQUIRE(visa_bad, "visa rejects 4-digit cvv");

        ValidateInput in_amex;
        in_amex.number = "378282246310005"; in_amex.cvv = "123";
        in_amex.today = Date{2026, 5};
        bool amex_bad = false;
        for (const auto& i : validate_full(in_amex).issues)
            if (i.code == IssueCode::BadCvvLength) amex_bad = true;
        REQUIRE(amex_bad, "amex rejects 3-digit cvv");
    });

    test("validate_full: implausibly far-future expiry warning", []{
        ValidateInput in;
        in.number = "4929923142719262";
        in.expiry = "01/40";
        in.cvv    = "123";
        in.today  = Date{2026, 5};
        in.max_future_years = 12;
        Report r = validate_full(in);
        bool seen = false;
        for (const auto& i : r.issues)
            if (i.code == IssueCode::ImplausibleFutureExpiry) seen = true;
        REQUIRE(seen, "implausible expiry");
    });

    test("validate_full: BIN database cross-checks", []{
        const char* csv =
            "BIN,Brand,Type,Category,Issuer,IssuerPhone,IssuerUrl,isoCode2,isoCode3,CountryName\n"
            "492992,VISA,CREDIT,,TEYA,,,GB,GBR,UNITED KINGDOM\n"
            "555555,MASTERCARD,DEBIT,,SOMEBANK,,,US,USA,UNITED STATES\n";
        BinDatabase db;
        REQUIRE(db.load_from_memory(csv), "load");

        // Country mismatch.
        ValidateInput in;
        in.number = "4929923142719262";
        in.expiry = "12/30"; in.cvv = "123";
        in.expected_country_iso2 = "US";
        in.db = &db;
        in.today = Date{2026, 5};
        bool country_bad = false;
        for (const auto& i : validate_full(in).issues)
            if (i.code == IssueCode::DbCountryDisagrees) country_bad = true;
        REQUIRE(country_bad, "country disagrees");

        // Type mismatch.
        ValidateInput in2 = in;
        in2.expected_country_iso2 = "GB";
        in2.expected_type = CardType::Debit;
        bool type_bad = false;
        for (const auto& i : validate_full(in2).issues)
            if (i.code == IssueCode::DbTypeDisagrees) type_bad = true;
        REQUIRE(type_bad, "type disagrees");

        // Unknown BIN.
        ValidateInput in3;
        in3.number = "4111111111111111";
        in3.today = Date{2026, 5};
        in3.db = &db;
        bool unknown = false;
        for (const auto& i : validate_full(in3).issues)
            if (i.code == IssueCode::DbBinUnknown) unknown = true;
        REQUIRE(unknown, "db bin unknown");
    });

    test("validate_full: every generated card validates without errors", []{
        Options opt; opt.brand = find_brand("visa");
        opt.fixed_month = 12; opt.fixed_year = 30; opt.fixed_cvv = "123";
        opt.format = false;
        Collector c; generate(500, opt, std::ref(c));
        for (const auto& ln : split_lines(c.buf)) {
            auto parts = split_by(ln, ' ');
            ValidateInput in;
            in.number = parts[0]; in.expiry = parts[1]; in.cvv = parts[2];
            in.today = Date{2026, 5};
            Report r = validate_full(in);
            REQUIRE(!r.has_errors(), std::string("errors on: ") + ln);
        }
    });

    test("analyze_digits: detects sequence and run", []{
        auto a = analyze_digits("1234567890");
        REQUIRE(a.longest_seq >= 9, std::string("seq=") + std::to_string(a.longest_seq));
        auto b = analyze_digits("1111111111");
        REQUIRE(b.all_same, "all same");
        auto c = analyze_digits("4242424242");
        REQUIRE(c.unique_digits == 2, "two unique");
    });

    test("validator throughput: 100k cards/s", []{
        Options opt; opt.brand = find_brand("visa");
        opt.fixed_month = 12; opt.fixed_year = 30; opt.fixed_cvv = "123";
        opt.format = false;
        Collector c; generate(50000, opt, std::ref(c));
        auto lines = split_lines(c.buf);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (const auto& ln : lines) {
            auto parts = split_by(ln, ' ');
            ValidateInput in;
            in.number = parts[0]; in.expiry = parts[1]; in.cvv = parts[2];
            in.today = Date{2026, 5};
            (void)validate_full(in);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double per_sec = lines.size() / (ms / 1000.0);
        std::printf("     (%.0f validations/s)\n", per_sec);
        REQUIRE(per_sec >= 100000.0, "validator throughput too low");
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
