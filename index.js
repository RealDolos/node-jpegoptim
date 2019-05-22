"use strict";

const {_optimize, _dumpdct, _versions} = require("./build/Release/binding");

const StripNone = 0;
const StripMeta = 1 << 0;
const StripICC = 1 << 1;
const StripThumbnail = 1 << 2;

/**
 * Something bad happened
 */
class OptimizeError extends TypeError { }

Object.defineProperty(OptimizeError.prototype, "name", {
  value: "OptimizeError",
  enumerable: true
});


/**
 * Optimize some JPEG image in memory.
 * @param {Buffer} buf Buffer containing the JPEG to optimize
 * @param {Object} [options] Some options for you.
 * @param {Buffer|Number} [options.out]
 *   Either the output buffer to use. The result is the used slice.
 *   Or a number limitting the buffer size.
 *   If omitted, a new Buffer without upper bound for the size is returned.
 *   Please remember that this function might add JFIF headers in otherwise
 *   already optimized files, which can lead to slight growing in size.
 * @param {Boolean} [options.strip] Strip all meta data.
 * @param {Boolean} [options.stripICC] Strip all ICC profile data.
 * @param {Boolean} [options.stripThumbnail]
 *   Strip any EXIF thumbnail present in the image metadata. Requires that this
 *   module was compiled against libexif.
 * @returns {Promise<Buffer>} The optimized jpeg.
 *
 * @throws TypeError
 * @throws RangeError
 * @throws OptimizeError
 *
 * @property {OptimizeError} OptimizeError Reference to OptimizeError
 * @property {Object} versions Library version of libjpeg etc
 * @property {Boolean} supportsThumbnailStripping Does this build support it?
 */
async function optimize(buf, options = {}) {
  const {strip = false, stripICC = false, stripThumbnail = false} = options;
  let flags = StripNone;
  if (strip) {
    flags |= StripMeta;
  }
  if (stripICC) {
    flags |= StripICC;
  }
  if (stripThumbnail) {
    flags |= StripThumbnail;
  }

  let {out} = options;
  if (typeof out === "number") {
    out = Buffer.allocUnsafe(out);
  }
  try {
    if (out) {
      const len = await _optimize(buf, flags, out);
      return out.slice(0, len);
    }
    return await _optimize(buf, flags);
  }
  catch (ex) {
    const {stack, invalid = false} = ex;
    if (ex.name === "RangeError") {
      // eslint-disable-next-line no-ex-assign
      ex = new RangeError(ex.message || ex);
    }
    else if (ex.name === "TypeError") {
      // eslint-disable-next-line no-ex-assign
      ex = new TypeError(ex.message || ex);
    }
    else {
      // eslint-disable-next-line no-ex-assign
      ex = new OptimizeError(ex.message || ex);
    }
    ex.stack = stack || ex.stack;
    Object.defineProperty(ex, "invalid", {
      value: invalid,
      enumerable: true,
    });
    throw ex;
  }
}

/**
 * Dump DCT bytes of a jpeg.
 *
 * The callback function will be called multiple times, usually with
 * a line of blocks.
 * Please note that the buffer func will receive will be reused and thus change
 * between calls. As such, you must copy the buffer if you want to keep
 * the contents.
 *
 * @param {Buffer} buf Buffer containing the JPEG to optimize
 * @param {Function} func Buffer receiving the DCT bytes
 *
 * @throws TypeError
 * @throws RangeError
 * @throws OptimizeError
 */
function dumpdct(buf, func) {
  try {
    if (typeof func !== "function") {
      throw new TypeError("func is not a function");
    }
    _dumpdct(buf, func);
  }
  catch (ex) {
    const {stack, invalid = false} = ex;
    if (ex.name === "RangeError") {
      // eslint-disable-next-line no-ex-assign
      ex = new RangeError(ex.message || ex);
    }
    else if (ex.name === "TypeError") {
      // eslint-disable-next-line no-ex-assign
      ex = new TypeError(ex.message || ex);
    }
    else {
      // eslint-disable-next-line no-ex-assign
      ex = new OptimizeError(ex.message || ex);
    }
    ex.stack = stack || ex.stack;
    Object.defineProperty(ex, "invalid", {
      value: invalid,
      enumerable: true,
    });
    throw ex;
  }
}


module.exports = Object.freeze(Object.assign(optimize, {
  optimize,
  dumpdct,
  OptimizeError,
  versions: _versions,
  supportsThumbnailStripping: "LIBEXIF_VERSION" in _versions,
}, _versions));
