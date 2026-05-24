'use strict';

const assert = require('assert');
const { generate, luhnCheck, BRANDS } = require('../src/generator.js');

function collect(count, opts = {}) {
  const chunks = [];
  generate(count, Object.assign({}, opts, { write: (c) => chunks.push(Buffer.from(c)) }));
  return Buffer.concat(chunks).toString('utf8');
}

function parseLines(s) {
  return s.split('\n').filter(Boolean);
}

let failed = 0;
function test(name, fn) {
  try { fn(); console.log('ok   ' + name); }
  catch (e) { failed++; console.log('FAIL ' + name + '\n     ' + e.message); }
}

test('generates exactly N lines', () => {
  const lines = parseLines(collect(1000));
  assert.strictEqual(lines.length, 1000);
});

test('every visa number passes Luhn and is 16 digits', () => {
  const lines = parseLines(collect(500, { brand: 'visa' }));
  for (const ln of lines) {
    const [card, exp, cvv] = ln.split(' ').reduce((acc, tok, idx, arr) => {
      // first N tokens belong to card until we hit MM/YY
      return acc;
    }, []) || [];
    // simpler: strip spaces then split known widths
    const parts = ln.split(' ');
    const cvvS = parts[parts.length - 1];
    const expS = parts[parts.length - 2];
    const digits = parts.slice(0, parts.length - 2).join('');
    assert.strictEqual(digits.length, 16, 'visa length: ' + ln);
    assert.ok(/^\d{16}$/.test(digits), 'visa digits: ' + ln);
    assert.ok(digits.startsWith('4'), 'visa prefix: ' + ln);
    assert.ok(luhnCheck(digits), 'visa luhn fail: ' + digits);
    assert.ok(/^\d{2}\/\d{2}$/.test(expS), 'expiry: ' + expS);
    const mm = parseInt(expS.slice(0, 2), 10);
    assert.ok(mm >= 1 && mm <= 12, 'month: ' + expS);
    assert.ok(/^\d{3}$/.test(cvvS), 'cvv: ' + cvvS);
  }
});

test('amex generates 15-digit numbers with 4-digit CVV', () => {
  const lines = parseLines(collect(300, { brand: 'amex' }));
  for (const ln of lines) {
    const parts = ln.split(' ');
    const cvvS = parts[parts.length - 1];
    const digits = parts.slice(0, parts.length - 2).join('');
    assert.strictEqual(digits.length, 15, 'amex length: ' + ln);
    assert.ok(digits.startsWith('34') || digits.startsWith('37'), 'amex prefix: ' + ln);
    assert.ok(luhnCheck(digits), 'amex luhn fail: ' + digits);
    assert.strictEqual(cvvS.length, 4, 'amex cvv: ' + cvvS);
  }
});

test('--no-format produces unspaced numbers', () => {
  const lines = parseLines(collect(100, { brand: 'mc', format: false }));
  for (const ln of lines) {
    const parts = ln.split(' ');
    assert.strictEqual(parts.length, 3, 'three fields: ' + ln);
    assert.strictEqual(parts[0].length, 16);
    assert.ok(luhnCheck(parts[0]), 'mc luhn fail: ' + parts[0]);
  }
});

test('all known brands work', () => {
  for (const name of Object.keys(BRANDS)) {
    const out = collect(20, { brand: name });
    assert.ok(out.length > 0, 'empty output for ' + name);
  }
});

if (failed) { console.error('\n' + failed + ' test(s) failed'); process.exit(1); }
console.log('\nall tests passed');
