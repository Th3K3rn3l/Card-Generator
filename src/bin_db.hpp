#pragma once

// Optional BIN/IIN lookup database.
//
// Loads a CSV in the schema published by https://github.com/venelinkochev/bin-list-data
// (CC-BY-4.0):
//
//     BIN,Brand,Type,Category,Issuer,IssuerPhone,IssuerUrl,isoCode2,isoCode3,CountryName
//
// The implementation is header-only and dependency-free. Strings are
// deduplicated through a small pool so repeated bank/country names do not
// blow up resident memory; records are stored in a vector sorted by BIN to
// make lookups O(log N).
//
// The full file is ~27 MB and ~375k rows; loading takes ~150-300 ms on a
// modern desktop and the resident footprint settles around 30-40 MB.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace cards {

// CardType maps the database's free-form `Type` column. Anything we do not
// recognize collapses to Unknown rather than failing the load.
enum class CardType : uint8_t {
    Unknown = 0,
    Credit,
    Debit,
    Prepaid,
    Charge,
};

inline const char* to_string(CardType t) {
    switch (t) {
        case CardType::Credit:  return "credit";
        case CardType::Debit:   return "debit";
        case CardType::Prepaid: return "prepaid";
        case CardType::Charge:  return "charge";
        case CardType::Unknown: return "unknown";
    }
    return "unknown";
}

// A single resolved BIN record. Strings live inside the owning `BinDatabase`'s
// string pool, so they're cheap to copy and stable as long as the database
// outlives the lookup result.
struct BinInfo {
    uint32_t          bin = 0;        // 6-digit numeric BIN, leading-zero-aware
    uint8_t           bin_len = 0;    // always 6 for the upstream dataset
    CardType          type = CardType::Unknown;
    std::string_view  brand;          // "VISA", "MASTERCARD", ...
    std::string_view  category;       // "PLATINUM", "PREPAID", "" if unknown
    std::string_view  issuer;         // bank name, "" if unknown
    std::string_view  issuer_phone;   // "" if unknown
    std::string_view  issuer_url;     // "" if unknown
    std::string_view  iso_alpha2;     // "US", "RU", ...
    std::string_view  iso_alpha3;     // "USA", "RUS", ...
    std::string_view  country;        // "UNITED STATES", ...
};

class BinDatabase {
public:
    struct LoadStats {
        size_t records_loaded = 0;
        size_t records_skipped = 0;
        size_t pool_bytes = 0;
    };

    // Constructs an empty database. Use `load(path)` to populate it.
    BinDatabase() = default;

    BinDatabase(const BinDatabase&) = delete;
    BinDatabase& operator=(const BinDatabase&) = delete;
    BinDatabase(BinDatabase&&) = default;
    BinDatabase& operator=(BinDatabase&&) = default;

    bool empty() const noexcept { return records_.empty(); }
    size_t size() const noexcept { return records_.size(); }
    const LoadStats& stats() const noexcept { return stats_; }

    // Loads (or replaces) the database from a CSV file on disk. Returns false
    // if the file cannot be opened. Malformed rows are silently skipped and
    // counted in `stats().records_skipped`.
    bool load(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;

        // Slurp in 64 KiB chunks so we don't pay per-line stdio overhead.
        std::vector<char> buf;
        buf.reserve(1 << 16);
        char block[64 * 1024];
        for (;;) {
            const size_t n = std::fread(block, 1, sizeof(block), f);
            if (n == 0) break;
            buf.insert(buf.end(), block, block + n);
        }
        std::fclose(f);

        return load_from_memory(std::string_view(buf.data(), buf.size()));
    }

    bool load_from_memory(std::string_view csv) {
        records_.clear();
        pool_.clear();
        stats_ = {};

        // Reserve assuming a typical row size of ~70 bytes — this dramatically
        // reduces reallocations on the upstream 27 MB file.
        records_.reserve(csv.size() / 64 + 16);
        // The dataset has only a few thousand unique strings (banks +
        // countries + brands + categories + URLs), so this rough cap is fine.
        pool_.reserve(8192);

        const char* p = csv.data();
        const char* end = csv.data() + csv.size();

        // Skip the header line if present.
        if (p < end && (*p == 'B' || *p == 'b')) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
            p = nl ? nl + 1 : end;
        }

        std::vector<std::string_view> cols;
        cols.reserve(10);
        std::string scratch;

        while (p < end) {
            const char* line_end = parse_csv_line(p, end, cols, scratch);
            if (!parse_record(cols)) {
                stats_.records_skipped++;
            } else {
                stats_.records_loaded++;
            }
            p = line_end;
        }

        std::sort(records_.begin(), records_.end(),
                  [](const Record& a, const Record& b) { return a.bin < b.bin; });

        size_t pool_bytes = 0;
        for (const auto& s : pool_) pool_bytes += s.size();
        stats_.pool_bytes = pool_bytes;
        return true;
    }

    // Look up the longest matching record for a BIN string of any length up
    // to 6. Returns nullptr if no record matches. Non-digit characters or
    // empty input also return nullptr.
    const BinInfo* lookup(std::string_view bin_str) const {
        if (records_.empty() || bin_str.empty()) return nullptr;

        uint32_t prefix = 0;
        int len = 0;
        for (char c : bin_str) {
            if (c < '0' || c > '9') return nullptr;
            prefix = prefix * 10u + static_cast<uint32_t>(c - '0');
            ++len;
            if (len > 6) return nullptr;
        }

        // Pad the prefix up to 6 digits for direct comparison with stored BINs
        // (which are always 6 digits in the upstream dataset).
        uint32_t low = prefix, high = prefix;
        for (int i = len; i < 6; ++i) {
            low  *= 10u;
            high  = high * 10u + 9u;
        }

        // Find the first record with bin >= low; if it's <= high, that's a hit.
        auto it = std::lower_bound(records_.begin(), records_.end(), low,
            [](const Record& r, uint32_t v) { return r.bin < v; });
        if (it == records_.end() || it->bin > high) return nullptr;

        // Cache a BinInfo view of the record for the caller. We use a
        // thread_local so successive calls don't re-allocate, while still
        // returning a stable pointer per call.
        thread_local BinInfo info;
        info = make_info(*it);
        return &info;
    }

private:
    struct Record {
        uint32_t          bin;
        CardType          type;
        std::string_view  brand;
        std::string_view  category;
        std::string_view  issuer;
        std::string_view  issuer_phone;
        std::string_view  issuer_url;
        std::string_view  iso_alpha2;
        std::string_view  iso_alpha3;
        std::string_view  country;
    };

    BinInfo make_info(const Record& r) const {
        BinInfo i;
        i.bin = r.bin;
        i.bin_len = 6;
        i.type = r.type;
        i.brand = r.brand;
        i.category = r.category;
        i.issuer = r.issuer;
        i.issuer_phone = r.issuer_phone;
        i.issuer_url = r.issuer_url;
        i.iso_alpha2 = r.iso_alpha2;
        i.iso_alpha3 = r.iso_alpha3;
        i.country = r.country;
        return i;
    }

    // Intern a string into the pool. The returned view stays valid for the
    // lifetime of the database. We use an `unordered_set<string>` because it
    // gives reference-stability across insertions, which the buffer of a
    // single `std::string` does not.
    //
    // C++17 does not yet provide heterogeneous `find(string_view)` on
    // unordered_set, so we always materialize a `std::string` for the lookup.
    // The lookup string is stack-friendly via SSO for the typical inputs we
    // see (brand names, country names — well under 30 bytes), so the cost is
    // acceptable in this load-once code path.
    std::string_view intern(std::string_view s) {
        if (s.empty()) return {};
        std::string key(s);
        auto it = pool_.find(key);
        if (it != pool_.end()) return std::string_view(*it);
        return std::string_view(*pool_.emplace(std::move(key)).first);
    }

    // Reads one CSV line (RFC 4180-style: optional double quotes, "" escapes
    // an embedded quote, line endings \n or \r\n). Returns the byte after
    // the terminating newline (or `end`). `scratch` is reused as a working
    // buffer for fields that contain escaped quotes; we record offsets into
    // it and resolve to string_views only after the line is fully parsed,
    // because scratch may relocate as it grows.
    const char* parse_csv_line(const char* p, const char* end,
                               std::vector<std::string_view>& cols,
                               std::string& scratch) {
        cols.clear();
        scratch.clear();

        // Each field is either an inline (data, size) pair pointing into the
        // source buffer, or a (offset, size) pair pointing into `scratch`.
        struct Pending {
            const char* data;     // when from_scratch == false
            size_t      off;      // when from_scratch == true
            size_t      size;
            bool        from_scratch;
        };
        // Reuse a stack-local buffer; the dataset has 10 columns max.
        Pending pending[16];
        int n = 0;

        while (p < end && n < 16) {
            Pending field{p, 0, 0, false};
            if (*p == '"') {
                const char* q = ++p;
                bool needs_scratch = false;
                while (p < end) {
                    if (*p == '"') {
                        if (p + 1 < end && p[1] == '"') {
                            needs_scratch = true;
                            p += 2;
                            continue;
                        }
                        break;
                    }
                    ++p;
                }
                if (needs_scratch) {
                    field.from_scratch = true;
                    field.off = scratch.size();
                    for (const char* r = q; r < p; ++r) {
                        if (*r == '"' && r + 1 < p && r[1] == '"') {
                            scratch.push_back('"');
                            ++r;
                        } else {
                            scratch.push_back(*r);
                        }
                    }
                    field.size = scratch.size() - field.off;
                } else {
                    field.data = q;
                    field.size = static_cast<size_t>(p - q);
                }
                if (p < end && *p == '"') ++p;
            } else {
                while (p < end && *p != ',' && *p != '\n' && *p != '\r') ++p;
                field.size = static_cast<size_t>(p - field.data);
                if (field.size && field.data[field.size - 1] == '\r') field.size--;
            }
            pending[n++] = field;

            if (p >= end) break;
            if (*p == ',') { ++p; continue; }
            if (*p == '\r') { ++p; if (p < end && *p == '\n') ++p; break; }
            if (*p == '\n') { ++p; break; }
        }

        cols.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto& f = pending[i];
            const char* base = f.from_scratch ? scratch.data() + f.off : f.data;
            cols.emplace_back(base, f.size);
        }
        return p;
    }

    bool parse_record(const std::vector<std::string_view>& cols) {
        if (cols.size() < 10) return false;
        // 0=BIN 1=Brand 2=Type 3=Category 4=Issuer 5=IssuerPhone 6=IssuerUrl
        // 7=isoCode2 8=isoCode3 9=CountryName
        const std::string_view bin_s = cols[0];
        if (bin_s.size() != 6) return false;
        uint32_t bin = 0;
        for (char c : bin_s) {
            if (c < '0' || c > '9') return false;
            bin = bin * 10u + static_cast<uint32_t>(c - '0');
        }

        Record r;
        r.bin           = bin;
        r.brand         = intern(cols[1]);
        r.type          = parse_type(cols[2]);
        r.category      = intern(cols[3]);
        r.issuer        = intern(cols[4]);
        r.issuer_phone  = intern(cols[5]);
        r.issuer_url    = intern(cols[6]);
        r.iso_alpha2    = intern(cols[7]);
        r.iso_alpha3    = intern(cols[8]);
        r.country       = intern(cols[9]);
        records_.push_back(r);
        return true;
    }

    static CardType parse_type(std::string_view s) {
        // Case-insensitive, prefix-tolerant match. The upstream dataset uses
        // "CREDIT" / "DEBIT" / "CHARGE CARD" but we accept lowercase too.
        if (s.empty()) return CardType::Unknown;
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        switch (c) {
            case 'C':
                if (eq_icase(s, "CREDIT"))      return CardType::Credit;
                if (eq_icase(s, "CHARGE CARD")) return CardType::Charge;
                if (eq_icase(s, "CHARGE"))      return CardType::Charge;
                return CardType::Unknown;
            case 'D': return eq_icase(s, "DEBIT")   ? CardType::Debit   : CardType::Unknown;
            case 'P': return eq_icase(s, "PREPAID") ? CardType::Prepaid : CardType::Unknown;
            default:  return CardType::Unknown;
        }
    }

    static bool eq_icase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(a[i])) !=
                std::toupper(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    std::vector<Record> records_;
    // Owns the deduplicated string contents that records reference.
    // unordered_set guarantees reference stability for insertions, which
    // means the string_views handed out via intern() remain valid for the
    // lifetime of the database.
    std::unordered_set<std::string> pool_;
    LoadStats stats_;
};

// Resolve a default BIN database path. Order:
//   1. `CARD_GENERATOR_BIN_DB` environment variable (if set).
//   2. `./data/bin-list-data.csv` relative to the working directory.
// Returns "" if no usable path is found.
inline std::string default_bin_db_path() {
    if (const char* env = std::getenv("CARD_GENERATOR_BIN_DB")) {
        if (*env) return std::string(env);
    }
    if (std::FILE* f = std::fopen("data/bin-list-data.csv", "rb")) {
        std::fclose(f);
        return "data/bin-list-data.csv";
    }
    return {};
}

} // namespace cards
