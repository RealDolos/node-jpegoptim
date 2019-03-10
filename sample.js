#!/usr/bin/env node
"use strict";

const {promises: {readFile, writeFile}} = require("fs");
// Actually import the module
const jopt = require("@dolos/jpegoptim");

async function main() {
  const [,, infile, outfile] = process.argv;
  if (!infile) {
    throw new Error("No input file provided");
  }
  if (!outfile) {
    throw new Error("No output file provided");
  }
  const input = await readFile(infile, {encoding: null});

  // Actually optimize
  const optimized = await jopt(input, {
    stripThumbnail: jopt.supportsThumbnailStripping
  });

  await writeFile(outfile, optimized, {encoding: null});
  const diff = input.byteLength - optimized.byteLength;
  console.log(
    infile, `(${input.byteLength})`,
    "->", outfile, `(${optimized.byteLength}`,
    diff, `(${(optimized.byteLength * 100 / input.byteLength).toFixed(2)}%)`
  );
}

main().then(() => process.exit(0)).catch(err => {
  err = err || new Error("Unknown Error");
  // jopt.OptimizeErrors are raised whenever the function was used correctly,
  // but the input data couldn't be processed
  if (err instanceof jopt.OptimizeError) {
    console.error("Oops:", err.message);
  }
  // Otherwise, e.g. when you use bad parameters, TypeError or RangeError may
  //be raised.
  else {
    console.error("You did something wrong:", err.message);
  }
  process.exit(1);
});
