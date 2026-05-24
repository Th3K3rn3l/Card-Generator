'use strict';

const BRANDS = {
  visa:     { prefixes: ['4'],                          length: 16, cvvLength: 3 },
  mc:        { prefixes: ['51','52','53','54','55'],     length: 16, cvvLength: 3 },
  mastercard:{ prefixes: ['51','52','53','54','55'],     length: 16, cvvLength: 3 },
  amex:      { prefixes: ['34','37'],                    length: 15, cvvLength: 4 },
  discover:  { prefixes: ['6011','65'],                  length: 16, cvvLength: 3 },
  jcb:       { prefixes: ['35'],                         length: 16, cvvLength: 3 },
};

function resolveBrand(name) {
  const cfg = BRANDS[(name || 'visa').toLowerCase()];
  if (!cfg) return null;
  return {
    prefixes: cfg.prefixes,
    prefDigits: cfg.prefixes.map(p => {
      const a = new Uint8Array(p.length);
      for (let i = 0; i < p.length; i++) a[i] = p.charCodeAt(i) - 48;
      return a;
    }),
    length: cfg.length,
    cvvLength: cfg.cvvLength,
  };
}

function luhnCheck(numStr) {
  let sum = 0;
  const len = numStr.length;
  for (let i = 0; i < len; i++) {
    let d = numStr.charCodeAt(len - 1 - i) - 48;
    if (d < 0 || d > 9) return false;
    if (i & 1) {
      d += d;
      if (d > 9) d -= 9;
    }
    sum += d;
  }
  return sum % 10 === 0;
}

function lineLength(cardLen, cvvLen, format, sepLen) {
  const cardStrLen = format ? cardLen + Math.ceil(cardLen / 4) - 1 : cardLen;
  return cardStrLen + sepLen + 5 + sepLen + cvvLen + 1;
}

function generate(count, opts = {}) {
  const brand = resolveBrand(opts.brand || 'visa');
  if (!brand) throw new Error(`unknown brand: ${opts.brand}`);

  const format = opts.format !== false;
  const separator = opts.separator != null ? opts.separator : ' ';
  const sepBuf = Buffer.from(separator, 'utf8');
  const sepLen = sepBuf.length;

  const totalLen = brand.length;
  const cvvLen = brand.cvvLength;
  const prefDigits = brand.prefDigits;
  const prefLen = prefDigits.length;

  const yearBase = (opts.now ? opts.now : new Date()).getFullYear() % 100;
  const yearSpan = opts.yearSpan || 6;

  const lineLen = lineLength(totalLen, cvvLen, format, sepLen);
  const BATCH = Math.min(count, Math.max(1, ((1 << 16) / lineLen) | 0));
  const buf = Buffer.allocUnsafe(BATCH * lineLen);

  const write = opts.write || ((chunk) => process.stdout.write(chunk));

  let remaining = count;
  while (remaining > 0) {
    const n = remaining < BATCH ? remaining : BATCH;
    let w = 0;
    for (let i = 0; i < n; i++) {
      const pref = prefDigits[(Math.random() * prefLen) | 0];
      const pLen = pref.length;

      let sum = 0;
      for (let j = 0; j < totalLen - 1; j++) {
        const d = j < pLen ? pref[j] : (Math.random() * 10) | 0;
        const pos = totalLen - j;
        if ((pos & 1) === 0) {
          const dd = d + d;
          sum += dd > 9 ? dd - 9 : dd;
        } else {
          sum += d;
        }
        if (format && j > 0 && (j & 3) === 0) buf[w++] = 0x20;
        buf[w++] = 48 + d;
      }
      const check = (10 - (sum % 10)) % 10;
      const j = totalLen - 1;
      if (format && j > 0 && (j & 3) === 0) buf[w++] = 0x20;
      buf[w++] = 48 + check;

      if (sepLen === 1) buf[w++] = sepBuf[0];
      else { sepBuf.copy(buf, w); w += sepLen; }

      const month = ((Math.random() * 12) | 0) + 1;
      buf[w++] = 48 + ((month / 10) | 0);
      buf[w++] = 48 + (month % 10);
      buf[w++] = 0x2f;
      const y2 = (yearBase + 1 + ((Math.random() * yearSpan) | 0)) % 100;
      buf[w++] = 48 + ((y2 / 10) | 0);
      buf[w++] = 48 + (y2 % 10);

      if (sepLen === 1) buf[w++] = sepBuf[0];
      else { sepBuf.copy(buf, w); w += sepLen; }

      for (let k = 0; k < cvvLen; k++) {
        buf[w++] = 48 + ((Math.random() * 10) | 0);
      }

      buf[w++] = 0x0a;
    }
    write(buf.subarray(0, w));
    remaining -= n;
  }
}

module.exports = { generate, luhnCheck, BRANDS, resolveBrand };
