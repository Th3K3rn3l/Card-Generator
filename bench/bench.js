'use strict';

const { generate } = require('../src/generator.js');

const COUNT = parseInt(process.argv[2], 10) || 1_000_000;

let bytes = 0;
const sink = (c) => { bytes += c.length; };

const t0 = process.hrtime.bigint();
generate(COUNT, { write: sink });
const t1 = process.hrtime.bigint();

const ms = Number(t1 - t0) / 1e6;
const perSec = (COUNT / (ms / 1000));
process.stderr.write(
  `generated ${COUNT.toLocaleString()} cards in ${ms.toFixed(1)} ms ` +
  `(${perSec.toFixed(0)} cards/s, ${(bytes / 1024 / 1024).toFixed(1)} MiB)\n`
);
