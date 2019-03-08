# node-jpegoptim
Optimize JPEGs in memory

## What

Losslessly transform jpegs to save some bytes. This is primarily done by telling libjpeg to optimize huffman codes, and by throwing away some garabge segments that another software might have inserted.

Also allows to strip metadata (EXIF, IPTC, XMP) segments and/or ICC profile segments (tho your colors will look bigly wrong).

## How

The API is really simple: there is only one function (default export of the module) and one custom error type.

`jpegoptim(buf, [options]`)
 * `@param {Buffer} buf` Buffer containing the JPEG to optimize
 * `@param {Object} [options] Some options for you.
 * `@param {Buffer|Number}` [options.out]
    Either the output buffer to use. The result is the used slice.
    Or a number limitting the buffer size.
    If omitted, a new Buffer without upper bound for the size is returned.
    Please remember that this function might add JFIF headers in otherwise
    already optimized files, which can lead to slight growing in size.
 * `@param {Boolean} [options.strip]` Strip all meta data.
 * `@param {Boolean} [options.stripICC]` Strip all ICC profile data.
 * `@returns {Promise<Buffer>}` The optimized jpeg.
 * `@throws TypeError`
 * `@throws RangeError`
 * `@throws OptimizeError`
 
 See [sample.js](sample.js) for a small program demonstrating the use.


## Design

 * Uses whatever your system libjpeg is (or what pkg-config said it was).
 * Offload to the node worker pool.
 * Avoid buffer memory copies. Operate directly on the input buffer. And either create an (external) output buffer, or operate directly on the user supplied output buffer.

## Todo

(as in: probably won't do)

 * Support Windows
 * Support systems where libjpeg cannot be pkg-config'ed
 * Support node versions I am not using.