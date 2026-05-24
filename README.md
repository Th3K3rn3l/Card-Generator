# card-generator

Fast CLI generator of Luhn-valid synthetic payment-card records.

Each record is one line: `<card-number> <MM/YY> <CVV>`.

> **Disclaimer.** Generated numbers pass the Luhn checksum and use real brand
> IIN prefixes, but they are **not issued to anyone**. They are intended for
> testing payment-handling software (form validation, parsers, masking,
> tokenisation pipelines, etc.) only. Do not attempt to use them for payments.

## Install

No dependencies. Requires Node.js >= 16.

```sh
node bin/generator.js 100
# or, after `npm link`:
generator 100
```

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
generator 100
generator 1000 --brand mc
generator 50000 --brand amex --no-format > cards.txt
generator 10 --sep , --no-format          # CSV-friendly
```

## Speed

The hot loop writes ASCII bytes directly into a pre-allocated `Buffer` (~64 KiB
batches) and flushes once per batch. No string concatenation, no regex, no
per-card allocations. Indicative throughput on a modern laptop core is **~3-5
million cards/sec** (sunk to /dev/null).

Benchmark:

```sh
node bench/bench.js 1000000 > /dev/null
```

## Tests

```sh
node test/test.js
```
