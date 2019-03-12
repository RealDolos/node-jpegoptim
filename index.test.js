"use strict";
/* globals test, describe, expect */

const optim = require("./");
const base = require("fs").readFileSync(`${__dirname}/test.jpg`, {encoding: null});

describe("globals", function() {
  test("function", function() {
    expect(optim).toBeDefined();
    expect(typeof optim).toBe("function");
    expect(optim.optimize).toBeDefined();
    expect(typeof optim.optimize).toBe("function");
  });

  test("OptimizeError", function() {
    expect(optim.OptimizeError).toBeDefined();
    expect(typeof optim.OptimizeError).toBe("function");
  });

  test("supportsThumbnailStripping", function() {
    expect(optim.supportsThumbnailStripping).toBeDefined();
    expect(typeof optim.supportsThumbnailStripping).toBe("boolean");
  });

  test("has versions", function() {
    expect(optim.versions).toBeDefined();
    expect(optim.versions.JPEG_VERSION).toBeDefined();
    expect(typeof optim.versions.JPEG_VERSION).toBe("string");
    expect(optim.versions.JPEG_COPYRIGHT).toBeDefined();
    expect(typeof optim.versions.JPEG_COPYRIGHT).toBe("string");
    if (optim.supportsThumbnailStripping) {
      expect(optim.versions.LIBEXIF_VERSION).toBeDefined();
      expect(typeof optim.versions.LIBEXIF_VERSION).toBe("string");
    }
  });
});


function ensure(opt) {
  expect(Buffer.isBuffer(opt)).toBe(true);
  expect(opt.length > 0).toBe(true);
  expect(opt.length < base.length).toBe(true);
  expect(!base.equals(opt)).toBe(true);
}

describe("optimize", function() {
  describe("bad params", function() {
    test("no params", async function() {
      await expect(optim()).rejects.toThrow(TypeError);
    });

    test("bad same buffer", async function() {
      const buf = Buffer.alloc(10);
      await expect(optim(buf, {out: buf})).rejects.toThrowError(RangeError);
    });

    test("types", async function() {
      await expect(optim("err")).rejects.toThrow(TypeError);
      await expect(optim("err")).rejects.toMatchObject({
        invalid: false,
      });
      await expect(optim(1)).rejects.toThrow(TypeError);
      await expect(optim(true)).rejects.toThrow(TypeError);
      await expect(optim(false)).rejects.toThrow(TypeError);
      await expect(optim(null)).rejects.toThrow(TypeError);
      await expect(optim(undefined)).rejects.toThrow(TypeError);
      await expect(optim(Buffer.alloc(0))).rejects.toThrow(TypeError);
    });
    test("invalid data", async function() {
      await expect(optim(Buffer.from("errror"))).
        rejects.toThrow(optim.OptimizeError);
      await expect(optim(Buffer.from("errror"))).rejects.toMatchObject({
        invalid: true,
        message: "Invalid image data",
      });
    });
  });

  describe("no param", function() {
    test("ok", async function() {
      const opt = await optim(base);
      ensure(opt);
    });
  });

  describe("num out", function() {
    test("ok", async function() {
      const opt = await optim(base, {out: base.length + 1024});
      ensure(opt);
    });
    test("small", async function() {
      await expect(optim(base, {out: 1024})).
        rejects.toThrow("Buffer too small");
      await expect(optim(base, {out: 1024})).
        rejects.toThrow(optim.OptimizeError);
    });
  });

  describe("buf out", function() {
    test("ok", async function() {
      const opt = await optim(base, {out: Buffer.alloc(base.length + 1024)});
      ensure(opt);
    });
    test("small", async function() {
      await expect(optim(base, {out: Buffer.alloc(1024)})).
        rejects.toThrow("Buffer too small");
      await expect(optim(base, {out: Buffer.alloc(1024)})).
        rejects.toThrow(optim.OptimizeError);
    });
  });

  describe("stripping", function() {
    test("strip works", async function() {
      const opt = await optim(base);
      const opt2 = await optim(base, {strip: true});
      ensure(opt2);
      expect(!opt.equals(opt2)).toBe(true);
    });

    test("stripICC works", async function() {
      const opt = await optim(base);
      const opt2 = await optim(base, {stripICC: true});
      ensure(opt2);
      expect(!opt.equals(opt2)).toBe(true);
    });

    test("stripThumbnail works", async function() {
      if (!optim.supportsThumbnailStripping) {
        this.skip();
        return;
      }

      const opt = await optim(base, {strip: true});
      const opt2 = await optim(base, {stripThumbnail: true});
      ensure(opt);
      ensure(opt2);
      expect(!opt.equals(opt2)).toBe(true);
    });
  });
});
