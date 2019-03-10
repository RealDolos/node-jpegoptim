#pragma once

#include <cinttypes>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <sys/types.h>
#include <unistd.h>

#include <jerror.h>
#include <jpeglib.h>

#ifdef HAS_EXIF
#  include <libexif/exif-data.h>
#endif

#include <nan.h>

#ifdef __GNUC__
#  pragma GCC visibility push(hidden)
#endif

namespace jpegoptim {
enum StripFlags : uint32_t {
  StripNone = 0,
  StripMeta = 1u << 0u,
  StripICC = 1u << 1u,
  StripThumbnail = 1u << 2u,
};

template<typename T>
struct free_deleter {
  void operator()(T* d) const
  {
    if (d != nullptr) {
      free(d);
    }
  }
};

#ifdef HAS_EXIF
struct ed {
  void operator()(ExifData* d) const
  {
    if (d != nullptr) {
      exif_data_unref(d);
    }
  }
};
#endif

class ErrorManager : public jpeg_error_mgr {
  std::string errmsg_{};
  bool ok_{true};
  bool invalid_{false};

  static void error(j_common_ptr info);

  static void message(j_common_ptr /* unused */) {}

 public:
  jmp_buf setjmp_buffer{};

  explicit ErrorManager() : jpeg_error_mgr{}
  {
    jpeg_std_error(this);
    error_exit = error;
    output_message = message;
  }

  explicit ErrorManager(const ErrorManager&) = delete;
  explicit ErrorManager(ErrorManager&&) = delete;
  ErrorManager& operator=(const ErrorManager&) = delete;
  ErrorManager& operator=(ErrorManager&&) = delete;

  ~ErrorManager() = default;

  explicit inline operator bool() const
  {
    return !ok_;
  }

  inline bool invalid() const
  {
    return invalid_;
  }

  inline const char* msg() const
  {
    return errmsg_.c_str();
  }
};

class Decompress : public jpeg_decompress_struct {
  bool inited_{false};

 public:
  explicit Decompress(
      ErrorManager* errmgr, const bool stripMeta, const bool stripICC)
      : jpeg_decompress_struct{}
  {
    err = errmgr;
    jpeg_create_decompress(static_cast<jpeg_decompress_struct*>(this));
    if (!stripMeta) {
      jpeg_save_markers(this, JPEG_APP0 + 1, 0xffff);  // EXIF / XMP
      jpeg_save_markers(this, JPEG_APP0 + 13, 0xffff);  // IPTC
    }
    if (!stripICC) {
      jpeg_save_markers(this, JPEG_APP0 + 2, 0xffff);  // ICC
    }
  }

  explicit Decompress(const Decompress&) = delete;
  explicit Decompress(Decompress&&) = delete;
  Decompress& operator=(const Decompress&) = delete;
  Decompress& operator=(Decompress&&) = delete;

  ~Decompress()
  {
    if (inited_) {
      jpeg_finish_decompress(this);
    }
    jpeg_destroy_decompress(this);
  }

  inline void init(const uint8_t* buffer, const size_t len)
  {
    jpeg_mem_src(this, buffer, len);
    jpeg_read_header(this, static_cast<boolean>(TRUE));
    inited_ = true;
  }
};

class MemoryDestination;

class Compress : public jpeg_compress_struct {
  std::unique_ptr<MemoryDestination> dst_;
  bool inited_{false};
  bool finished_{false};

 public:
  explicit Compress(Decompress& dec, size_t memhint);
  explicit Compress(Decompress& dec, uint8_t* buffer, size_t capacity);

  explicit Compress(const Compress&) = delete;
  explicit Compress(Compress&&) = delete;
  Compress& operator=(const Compress&) = delete;
  Compress& operator=(Compress&&) = delete;

  ~Compress()
  {
    jpeg_destroy_compress(this);
  }

  inline void Init(jvirt_barray_ptr* coefs)
  {
    jpeg_write_coefficients(this, coefs);
    inited_ = true;
  }

  inline void Finish()
  {
    if (!inited_ || finished_) {
      return;
    }
    jpeg_finish_compress(this);
    finished_ = true;
  }

  inline std::unique_ptr<MemoryDestination> Buffer()
  {
    dest = nullptr;
    return std::move(dst_);
  }

  inline MemoryDestination* Dest()
  {
    return dst_.get();
  }
};

class MemoryDestination : public jpeg_destination_mgr {
  static void init(j_compress_ptr compress);
  static boolean empty(j_compress_ptr compress);
  static void term(j_compress_ptr compress);

 protected:
  size_t size_{0};
  size_t capacity_;

 public:
  explicit MemoryDestination(const size_t capacity)
      : jpeg_destination_mgr{}, capacity_{capacity}
  {
    init_destination = init;
    empty_output_buffer = empty;
    term_destination = term;
  }
  MemoryDestination(const MemoryDestination&) = delete;
  MemoryDestination(MemoryDestination&&) = delete;
  MemoryDestination& operator=(const MemoryDestination&) = delete;
  MemoryDestination& operator=(MemoryDestination&&) = delete;

  virtual ~MemoryDestination() = default;

 protected:
  virtual void Init() = 0;
  virtual boolean Empty() = 0;
  virtual void Term() = 0;

 public:
  inline size_t Length() const
  {
    return size_;
  }

  inline size_t Capacity() const
  {
    return capacity_;
  }

  static void destroy(char* /* unused */, void* /* unused */) {}

 public:
  virtual uint8_t* Data() = 0;

  virtual bool Managed() const = 0;
};

class ManagedMemoryDestination : public MemoryDestination {
  static constexpr size_t buffer_growth{1u << 14u};

  std::unique_ptr<uint8_t, free_deleter<uint8_t>> buffer_;

 protected:
  inline void Init() final
  {
    next_output_byte =
        reinterpret_cast<decltype(next_output_byte)>(buffer_.get()) + size_;
    free_in_buffer = capacity_;
  }

  boolean Empty() final;
  void Term() final;

 public:
  explicit ManagedMemoryDestination(size_t memhint) : MemoryDestination(0)
  {
    memhint = ((memhint / buffer_growth) + 1) * buffer_growth;
    buffer_.reset(reinterpret_cast<uint8_t*>(malloc(memhint)));
    capacity_ = buffer_ ? memhint : 0;
  }
  ManagedMemoryDestination(const ManagedMemoryDestination&) = delete;
  ManagedMemoryDestination(ManagedMemoryDestination&&) = delete;
  ManagedMemoryDestination& operator=(const ManagedMemoryDestination&) = delete;
  ManagedMemoryDestination& operator=(ManagedMemoryDestination&&) = delete;

  ~ManagedMemoryDestination() final = default;

  uint8_t* Data() final
  {
    return buffer_.get();
  }

  bool Managed() const final
  {
    return true;
  }
};

class UnmanagedMemoryDestination : public MemoryDestination {
  uint8_t* buffer_;

 protected:
  void Init() final
  {
    next_output_byte =
        reinterpret_cast<decltype(next_output_byte)>(buffer_) + size_;
    free_in_buffer = capacity_;
  }

  boolean Empty() final
  {
    // Cannot "empty" the buffer, as this would mean resizing it
    return static_cast<boolean>(FALSE);
  }

  void Term() final
  {
    size_ = capacity_ - free_in_buffer;
  }

 public:
  explicit UnmanagedMemoryDestination(uint8_t* buffer, const size_t capacity)
      : MemoryDestination(capacity), buffer_{buffer}
  {
  }
  UnmanagedMemoryDestination(const UnmanagedMemoryDestination&) = delete;
  UnmanagedMemoryDestination(UnmanagedMemoryDestination&&) = delete;
  UnmanagedMemoryDestination& operator=(const UnmanagedMemoryDestination&) =
      delete;
  UnmanagedMemoryDestination& operator=(UnmanagedMemoryDestination&&) = delete;

  ~UnmanagedMemoryDestination() final = default;

  uint8_t* Data() final
  {
    return buffer_;
  }

  bool Managed() const final
  {
    return false;
  }
};

class Optimizer : public Nan::AsyncWorker {
  std::unique_ptr<Compress> compress_;

  const uint8_t* buffer_;
  const size_t len_;

  uint8_t* outbuf_{};
  size_t outlen_{};

#ifdef HAS_EXIF
  std::string replacementExif{};
#endif

  bool invalid_{false};

#ifdef HAS_EXIF
  bool stripThumb_;
#endif
  bool stripMeta_;
  bool stripICC_;

  bool CopyMarkers(ErrorManager& err, Decompress& dec);

 public:
  explicit Optimizer(
      v8::Local<v8::Promise::Resolver>& res,
      v8::Local<v8::ArrayBufferView>& buf,
      v8::MaybeLocal<v8::ArrayBufferView>& outbuf,
      StripFlags flags);

  explicit Optimizer(const Optimizer&) = delete;
  explicit Optimizer(Optimizer&&) = delete;
  Optimizer& operator=(const Optimizer&) = delete;
  Optimizer& operator=(Optimizer&&) = delete;

  ~Optimizer() final = default;

  void Execute() final;
  void HandleOKCallback() final;
  void HandleErrorCallback() final;
};

}  // namespace jpegoptim

#ifdef __GNUC__
#  pragma GCC visibility pop
#endif

