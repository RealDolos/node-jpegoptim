#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <inttypes.h>
#include <setjmp.h>
#include <sys/types.h>
#include <unistd.h>

#include <jerror.h>
#include <jpeglib.h>

#include <nan.h>

using v8::ArrayBufferView;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::Persistent;
using v8::Promise;
using v8::WeakCallbackInfo;
using v8::WeakCallbackType;

namespace {

constexpr const char TAG_EXIF[] = "Exif\0\0";
constexpr const size_t TAG_EXIF_LEN = 6;

constexpr const char TAG_XMP[] = "http://ns.adobe.com/xap/1.0/\0";
constexpr const size_t TAG_XMP_LEN = 29;

constexpr const char TAG_ICC[] = "ICC_PROFILE\0";
constexpr const size_t TAG_ICC_LEN = 12;

constexpr const char TAG_IPTC[] = "\0x1c";
constexpr const size_t TAG_IPTC_LEN = 1;

enum StripFlags : uint32_t {
  StripNone = 0,
  StripMeta = 1 << 0,
  StripICC = 1 << 1,
};

class ErrorManager : public jpeg_error_mgr {
  std::string errmsg_{};
  bool ok_{true};

  static void error(j_common_ptr info)
  {
    ErrorManager* err = reinterpret_cast<ErrorManager*>(info->err);
    if (!err) {
      return;
    }
    err->ok_ = false;
    if (err->msg_code == JERR_CANT_SUSPEND) {
      err->errmsg_ = "Buffer too small";
    }
    else {
      char buffer[JMSG_LENGTH_MAX];
      *buffer = 0;
      err->format_message(info, buffer);
      err->errmsg_ = buffer;
    }
    longjmp(err->setjmp_buffer, 1);
  }

  static void message(j_common_ptr info) {}

 public:
  jmp_buf setjmp_buffer;

  explicit ErrorManager()
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

  inline operator bool() const
  {
    return ok_;
  }

  inline const char* msg() const
  {
    return errmsg_.c_str();
  }
};

class Decompress : public jpeg_decompress_struct {
  bool inited_{false};

 public:
  explicit Decompress(ErrorManager* errmgr, const StripFlags flags)
  {
    jpeg_create_decompress(this);
    if ((flags & StripMeta) != StripMeta) {
      jpeg_save_markers(this, JPEG_APP0 + 1, 0xffff);  // EXIF / XMP
      jpeg_save_markers(this, JPEG_APP0 + 13, 0xffff);  // IPTC
    }
    if ((flags & StripICC) != StripICC) {
      jpeg_save_markers(this, JPEG_APP0 + 2, 0xffff);  // ICC
    }
    err = errmgr;
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

  void init(const uint8_t* buffer, size_t len)
  {
    jpeg_mem_src(this, buffer, len);
    jpeg_read_header(this, 1);
    inited_ = true;
  }
};

class MemoryDestination : public jpeg_destination_mgr {
  static constexpr size_t buffer_grows{1 << 14};

  uint8_t* buffer_;
  size_t size_{0};
  size_t capacity_;
  const bool managed_;

  static void init(j_compress_ptr compress)
  {
    const auto dest = reinterpret_cast<MemoryDestination*>(compress->dest);
    if (!dest) {
      return;
    }
    dest->next_output_byte = dest->buffer_ + dest->size_;
    dest->free_in_buffer = dest->capacity_;
  }

  static int empty(j_compress_ptr compress)
  {
    const auto dest = reinterpret_cast<MemoryDestination*>(compress->dest);
    if (!dest) {
      return 0;
    }
    if (!dest->managed_) {
      return 0;
    }
    dest->size_ = dest->capacity_ - dest->free_in_buffer;
    auto newcap = dest->capacity_ + buffer_grows;
    auto newbuf = (uint8_t*)realloc(dest->buffer_, newcap);
    if (!newbuf) {
      return 0;
    }
    dest->buffer_ = newbuf;
    dest->capacity_ = newcap;
    dest->next_output_byte = dest->buffer_ + dest->size_;
    dest->free_in_buffer = buffer_grows;
    return 1;
  }

  static void term(j_compress_ptr compress)
  {
    const auto dest = reinterpret_cast<MemoryDestination*>(compress->dest);
    if (!dest) {
      return;
    }
    dest->size_ = dest->capacity_ - dest->free_in_buffer;
    if (!dest->managed_) {
      return;
    }
    if (dest->free_in_buffer < buffer_grows) {
      return;
    }
    auto newbuf = (uint8_t*)realloc(dest->buffer_, dest->size_);
    if (!newbuf) {
      return;
    }
    dest->buffer_ = newbuf;
    dest->capacity_ = dest->size_;
    dest->free_in_buffer = 0;
  }

 public:
  explicit MemoryDestination(size_t memhint) : managed_{true}
  {
    init_destination = init;
    empty_output_buffer = empty;
    term_destination = term;
    memhint = ((memhint / buffer_grows) + 1) * buffer_grows;
    buffer_ = (uint8_t*)malloc(memhint);
    capacity_ = buffer_ ? memhint : 0;
  }

  explicit MemoryDestination(uint8_t* buffer, size_t capacity)
      : buffer_{buffer}, capacity_{capacity}, managed_{false}
  {
    init_destination = init;
    empty_output_buffer = empty;
    term_destination = term;
  }

  explicit MemoryDestination(const MemoryDestination&) = delete;
  explicit MemoryDestination(MemoryDestination&&) = delete;
  MemoryDestination& operator=(const MemoryDestination&) = delete;
  MemoryDestination& operator=(MemoryDestination&&) = delete;

  ~MemoryDestination()
  {
    if (!buffer_) {
      return;
    }
    if (managed_) {
      free(buffer_);
    }
    buffer_ = nullptr;
    size_ = 0;
    capacity_ = 0;
  }

  inline const uint8_t* data() const
  {
    return buffer_;
  }

  inline size_t length() const
  {
    return size_;
  }

  inline size_t capacity() const
  {
    return capacity_;
  }

  inline bool managed() const
  {
    return managed_;
  }

  static void destroy(char*, void* hint) {}
};

class Compress : public jpeg_compress_struct {
  std::unique_ptr<MemoryDestination> dst_;
  bool inited_{false};
  bool finished_{false};

 public:
  explicit Compress(Decompress& dec, size_t memhint)
      : dst_{std::make_unique<MemoryDestination>(memhint)}
  {
    jpeg_create_compress(this);

    err = dec.err;
    jpeg_copy_critical_parameters(&dec, this);
    progressive_mode = FALSE;
    optimize_coding = 1;
    dest = dst_.get();
  }

  explicit Compress(Decompress& dec, uint8_t* buffer, size_t capacity)
      : dst_{std::make_unique<MemoryDestination>(buffer, capacity)}
  {
    jpeg_create_compress(this);

    err = dec.err;
    jpeg_copy_critical_parameters(&dec, this);
    progressive_mode = FALSE;
    optimize_coding = 1;
    dest = dst_.get();
  }

  explicit Compress(const Compress&) = delete;
  explicit Compress(Compress&&) = delete;
  Compress& operator=(const Compress&) = delete;
  Compress& operator=(Compress&&) = delete;

  ~Compress()
  {
    jpeg_destroy_compress(this);
  }

  void init(jvirt_barray_ptr* coefs)
  {
    jpeg_write_coefficients(this, coefs);
    inited_ = true;
  }

  void finish()
  {
    if (!inited_ || finished_) {
      return;
    }
    jpeg_finish_compress(this);
    finished_ = true;
  }

  std::unique_ptr<MemoryDestination> buffer()
  {
    dest = nullptr;
    return std::move(dst_);
  }
};

class Holder {
  Persistent<Object> persistent_;
  std::unique_ptr<MemoryDestination> dest_;
  const size_t self_;

  void Reset(Isolate* isolate)
  {
    const auto freed = -static_cast<int64_t>(self_);
    isolate->AdjustAmountOfExternalAllocatedMemory(freed);
    delete this;
  }

  static void WeakCallback(const WeakCallbackInfo<Holder>& info)
  {
    info.GetParameter()->Reset(info.GetIsolate());
  }

 public:
  explicit Holder(
      Isolate* isolate,
      Local<Object>& o,
      std::unique_ptr<MemoryDestination>&& dest)
      : persistent_(isolate, o),
        dest_{std::move(dest)},
        self_{sizeof(*this) + sizeof(MemoryDestination) + dest_->capacity()}
  {
    persistent_.SetWeak(this, WeakCallback, WeakCallbackType::kParameter);
    isolate->AdjustAmountOfExternalAllocatedMemory(self_);
  }

  explicit Holder(const Holder&) = delete;
  explicit Holder(Holder&&) = delete;
  Holder& operator=(const Holder&) = delete;
  Holder& operator=(Holder&&) = delete;

  ~Holder()
  {
    persistent_.Reset();
    dest_.reset();
  }
};

class Optimizer : public Nan::AsyncWorker {
  std::unique_ptr<Compress> compress_;

  const uint8_t* buffer_;
  const size_t len_;

  uint8_t* outbuf_{};
  size_t outlen_{};

  const StripFlags flags_;

  static uint8_t* Data(Local<ArrayBufferView>& buffer)
  {
    auto d = buffer->Buffer()->GetContents().Data();
    return reinterpret_cast<uint8_t*>(d) + buffer->ByteOffset();
  }

 public:
  explicit Optimizer(
      Local<Promise::Resolver>& res,
      Local<ArrayBufferView>& buf,
      MaybeLocal<ArrayBufferView>& outbuf,
      StripFlags flags)
      : Nan::AsyncWorker(nullptr, "jpegoptimize"),
        buffer_{Data(buf)},
        len_{buf->ByteLength()},
        flags_{flags}
  {
    SaveToPersistent("buf", buf);
    SaveToPersistent("res", res);
    if (!outbuf.IsEmpty()) {
      auto obuf = outbuf.ToLocalChecked();
      SaveToPersistent("out", obuf);
      outbuf_ = Data(obuf);
      outlen_ = obuf->ByteLength();
    }
  }

  explicit Optimizer(const Optimizer&) = delete;
  explicit Optimizer(Optimizer&&) = delete;
  Optimizer& operator=(const Optimizer&) = delete;
  Optimizer& operator=(Optimizer&&) = delete;

  bool CopyMarkers(ErrorManager& err, Decompress& dec)
  {
    const auto stripMeta = (flags_ & StripMeta) == StripMeta;
    const auto stripICC = (flags_ & StripICC) == StripICC;
    auto marker = dec.marker_list;
    auto sawICC{false};
    std::vector<decltype(marker)> mrks;
    while (marker) {
      switch (marker->marker) {
      case JPEG_APP0 + 1:
        if (stripMeta) {
          break;
        }
        if (marker->data_length > TAG_EXIF_LEN &&
            !memcmp(marker->data, TAG_EXIF, TAG_EXIF_LEN)) {
          mrks.push_back(marker);
        }
        else if (
            marker->data_length > TAG_XMP_LEN &&
            !memcmp(marker->data, TAG_XMP, TAG_XMP_LEN)) {
          mrks.push_back(marker);
        }
        break;

      case JPEG_APP0 + 2:
        if (stripICC) {
          break;
        }
        if (sawICC ||
            (marker->data_length > TAG_ICC_LEN &&
             !memcmp(marker->data, TAG_ICC, TAG_ICC_LEN))) {
          sawICC = true;
          mrks.push_back(marker);
        }
        break;

      case JPEG_APP0 + 13:
        if (stripMeta) {
          break;
        }
        if (marker->data_length > TAG_IPTC_LEN &&
            !memcmp(marker->data, TAG_IPTC, TAG_IPTC_LEN)) {
          mrks.push_back(marker);
        }
        break;

      default:
        // no copy mr roboto
        break;
      }
      marker = marker->next;
    }

    std::sort(
        mrks.begin(), mrks.end(), [](decltype(marker) a, decltype(marker) b) {
          return a->marker < b->marker;
        });

    for (const auto& m : mrks) {
      jpeg_write_marker(compress_.get(), m->marker, m->data, m->data_length);
      if (!err) {
        compress_.reset();
        SetErrorMessage(err.msg());
        return false;
      }
    }

    return true;
  }

  void Execute() final
  {
    ErrorManager err;
    Decompress dec(&err, flags_);
    if (setjmp(err.setjmp_buffer)) {
      if (!err) {
        return SetErrorMessage(err.msg());
      }
      return SetErrorMessage("Invalid Image");
    }

    dec.init(buffer_, len_);
    auto coefs = jpeg_read_coefficients(&dec);
    if (!err) {
      return SetErrorMessage(err.msg());
    }
    if (!coefs) {
      return SetErrorMessage("Invalid image");
    }

    if (outbuf_) {
      compress_ = std::make_unique<Compress>(dec, outbuf_, outlen_);
    }
    else {
      compress_ = std::make_unique<Compress>(dec, len_);
    }
    compress_->init(coefs);
    if (!err) {
      compress_.reset();
      SetErrorMessage(err.msg());
      return;
    }

    if (!CopyMarkers(err, dec)) {
      return;
    }

    compress_->finish();
    if (!err) {
      compress_.reset();
      SetErrorMessage(err.msg());
      return;
    }
  }

  void HandleOKCallback() final
  {
    Nan::HandleScope scope;
    GetFromPersistent("buf");
    GetFromPersistent("out");
    auto resolver = GetFromPersistent("res").As<Promise::Resolver>();
    if (!compress_) {
      auto err = Nan::Error("Unknown error");
      resolver->Reject(Nan::GetCurrentContext(), err).IsNothing();
      return;
    }
    auto dest = compress_->buffer();
    compress_.reset();
    auto isolate = Isolate::GetCurrent();

    if (!dest->managed()) {
      auto length = Nan::New<Number>((double)dest->length());
      resolver->Resolve(Nan::GetCurrentContext(), length).IsNothing();
      dest.reset();
      return;
    }

    auto buf = node::Buffer::New(
        isolate, (char*)dest->data(), dest->length(),
        MemoryDestination::destroy, dest.get());
    if (buf.IsEmpty()) {
      auto err = Nan::Error("Cannot create output buffer");
      resolver->Reject(Nan::GetCurrentContext(), err).IsNothing();
      return;
    }

    auto lbuf = buf.ToLocalChecked();
    new Holder(isolate, lbuf, std::move(dest));
    resolver->Resolve(Nan::GetCurrentContext(), lbuf).IsNothing();
  }

  void HandleErrorCallback() final
  {
    Nan::HandleScope scope;
    GetFromPersistent("buf");
    GetFromPersistent("out");
    auto resolver = GetFromPersistent("res").As<Promise::Resolver>();
    auto err = Nan::Error(ErrorMessage());
    resolver->Reject(Nan::GetCurrentContext(), err).IsNothing();
  }
};

}  // namespace

NAN_METHOD(optimize)
{
  Nan::HandleScope scope;
  if (info.Length() < 2 || !node::Buffer::HasInstance(info[0])) {
    return Nan::ThrowTypeError("Expected a buffer and flags");
  }

  if (!info[0]->IsArrayBufferView()) {
    return Nan::ThrowTypeError("Expected a buffer");
  }
  auto buf = info[0].As<ArrayBufferView>();
  if (buf->ByteLength() <= 0) {
    return Nan::ThrowTypeError("Expected a filled buffer");
  }

  const auto flags = (StripFlags)Nan::To<uint32_t>(info[1]).FromJust();

  MaybeLocal<ArrayBufferView> outbuf;
  if (info.Length() > 2) {
    if (!info[2]->IsArrayBufferView()) {
      return Nan::ThrowTypeError("Expected an output buffer");
    }
    auto lobuf = info[2].As<ArrayBufferView>();
    if (lobuf == buf) {
      return Nan::ThrowRangeError("Input ant output buffer cannot be the same");
    }
    if (lobuf->ByteLength() <= 0) {
      return Nan::ThrowTypeError("Expected a non-zero output buffer");
    }
    outbuf = lobuf;
  }

  auto resolver =
      Promise::Resolver::New(Nan::GetCurrentContext()).ToLocalChecked();
  auto promise = resolver->GetPromise();
  Nan::AsyncQueueWorker(new Optimizer(resolver, buf, outbuf, flags));
  info.GetReturnValue().Set(promise);
}

NAN_MODULE_INIT(InitAll)
{
  Nan::Set(
      target, Nan::New("_optimize").ToLocalChecked(),
      Nan::GetFunction(Nan::New<FunctionTemplate>(optimize)).ToLocalChecked());
}

NODE_MODULE(binding, InitAll)
