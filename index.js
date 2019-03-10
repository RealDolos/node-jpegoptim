"use strict";

const {_optimize, _versions} = require("./build/Release/binding");

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
    const {invalid = false} = ex;
    if (!(ex instanceof RangeError || ex instanceof TypeError)) {
      // eslint-disable-next-line no-ex-assign
      ex = new OptimizeError(ex.message || ex);
    }
    Object.defineProperty(ex, "invalid", {
      value: invalid,
      enumerable: true,
    });
    throw ex;
  }
}

module.exports = Object.freeze(Object.assign(optimize, {
  optimize,
  OptimizeError,
  versions: _versions,
  supportsThumbnailStripping: "LIBEXIF_VERSION" in _versions,
}, _versions));
