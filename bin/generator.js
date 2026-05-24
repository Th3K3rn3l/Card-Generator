#!/usr/bin/env node
'use strict';

const { generate, BRANDS } = require('../src/generator.js');

function printUsage(stream) {
  stream.write(
    'Usage: generator <count> [options]\n' +
    '\n' +
    'Generates <count> Luhn-valid synthetic card records.\n' +
    'Each line: <card-number> <MM/YY> <CVV>\n' +
    '\n' +
    'Options:\n' +
    '  -b, --brand <name>   Card brand: ' + Object.keys(BRANDS).join(', ') + ' (default: visa)\n' +
    '      --no-format      Do not insert spaces between groups of digits\n' +
    '      --sep <s>        Field separator (default: single space)\n' +
    '      --year-span <n>  Years of validity range above current (default: 6)\n' +
    '  -h, --help           Show this help\n' +
    '\n' +
    'Examples:\n' +
    '  generator 100\n' +
    '  generator 1000 --brand mc\n' +
    '  generator 50000 --brand amex --no-format\n'
  );
}

const argv = process.argv;
let count = 0;
const opts = {};

for (let i = 2; i < argv.length; i++) {
  const a = argv[i];
  if (a === '-h' || a === '--help') { printUsage(process.stdout); process.exit(0); }
  else if (a === '-b' || a === '--brand') opts.brand = argv[++i];
  else if (a === '--no-format') opts.format = false;
  else if (a === '--sep') opts.separator = argv[++i];
  else if (a === '--year-span') opts.yearSpan = parseInt(argv[++i], 10) | 0;
  else if (count === 0 && /^\d+$/.test(a)) count = parseInt(a, 10);
  else {
    process.stderr.write(`generator: unknown argument: ${a}\n`);
    printUsage(process.stderr);
    process.exit(2);
  }
}

if (!count || count < 1) {
  printUsage(process.stderr);
  process.exit(1);
}

try {
  generate(count, opts);
} catch (e) {
  process.stderr.write(`generator: ${e.message}\n`);
  process.exit(1);
}
