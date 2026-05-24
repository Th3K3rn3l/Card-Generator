# card-generator

Fast CLI generator of Luhn-valid synthetic payment-card records, written in
C++17. Single executable, no runtime dependencies.

Each record is one line: `<card-number> <MM/YY> <CVV>`.

> **Disclaimer.** Generated numbers pass the Luhn checksum and use real brand
> IIN prefixes, but they are **not issued to anyone**. They are intended for
> testing payment-handling software (form validation, parsers, masking,
> tokenisation pipelines, etc.) only. Do not attempt to use them for payments.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Optionally tune for the host CPU (less portable binary):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGENERATOR_NATIVE=ON
cmake --build build -j
```

The resulting binary is `build/generator`.

## Usage

```
generator <count> [options]

Options:
  -b, --brand <name>   visa | mc | amex | discover | jcb (default: visa)
      --no-format      Do not insert spaces between groups of 4 digits
      --sep <s>        Field separator (default: single space)
      --year-span <n>  Years of validity range above current (default: 6)
  -h, --help           Show help
```

Examples:

```sh
./build/generator 100
./build/generator 1000 --brand mc
./build/generator 50000 --brand amex --no-format > cards.txt
./build/generator 10 --sep , --no-format          # CSV-friendly
```

## Speed

The hot loop writes ASCII bytes directly into a 64 KiB pre-allocated buffer
and flushes once per batch through a 1 MiB `setvbuf`-backed stdout buffer.
No allocations per card, no `std::string`, no streams. Randomness comes from
xoshiro256\*\* seeded from `std::random_device`; bounded ranges use Lemire's
multiplication trick (no `%`).

Expected throughput on a single modern core: **~10–30 million cards/sec** sunk
to `/dev/null` (varies by CPU and brand).

## Tests

```sh
cmake --build build --target test_cards
./build/test_cards
# or, via CTest:
ctest --test-dir build --output-on-failure
```

The test binary verifies Luhn validity, brand prefixes, field widths, and
checks throughput (≥ 1M cards/s on the build host).
