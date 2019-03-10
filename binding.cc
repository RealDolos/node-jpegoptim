#include <algorithm>
#include <vector>

#include "binding.hh"

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

#ifdef __GNUC__
#  pragma GCC visibility push(hidden)
#endif

namespace {
constexpr const char TAG_EXIF[] = "Exif\0\0";
constexpr const size_t TAG_EXIF_LEN = sizeof(TAG_EXIF);

constexpr const char TAG_XMP[] = "http://ns.adobe.com/xap/1.0/\0";
constexpr const size_t TAG_XMP_LEN = sizeof(TAG_XMP);

constexpr const char TAG_ICC[] = "ICC_PROFILE\0";
constexpr const size_t TAG_ICC_LEN = sizeof(TAG_ICC);

constexpr const char TAG_IPTC[] = "\x1c";
constexpr const size_t TAG_IPTC_LEN = sizeof(TAG_IPTC);

uint8_t* BufferData(Local<ArrayBufferView>& buffer)
{
  auto d = buffer->Buffer()->GetContents().Data();
  return reinterpret_cast<uint8_t*>(d) + buffer->ByteOffset();
}

template<class T>
class Holder {
  Persistent<Object> persistent_;
  std::unique_ptr<T> dest_;
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
  explicit Holder(Isolate* isolate, Local<Object>& o, std::unique_ptr<T>&& dest)
      : persistent_(isolate, o),
        dest_{std::move(dest)},
        self_{sizeof(*this) + sizeof(T) + dest_->Capacity()}
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

}  // namespace

namespace jpegoptim {
void ErrorManager::error(j_common_ptr info)
{
  const auto err = reinterpret_cast<ErrorManager*>(info->err);
  if (err == nullptr) {
    return;
  }
  err->ok_ = false;
  switch (err->msg_code) {
  case JERR_NO_HUFF_TABLE:
  case JERR_NO_IMAGE:
  case JERR_NO_QUANT_TABLE:
  case JERR_NO_SOI:
    err->errmsg_ = "Invalid image data";
    err->invalid_ = true;
    break;
  case JERR_CANT_SUSPEND:
    err->errmsg_ = "Buffer too small";
    break;
  default: {
    char buffer[JMSG_LENGTH_MAX];
    *buffer = 0;
    err->format_message(info, buffer);
    err->errmsg_ = buffer;
    break;
  }
  }
  longjmp(err->setjmp_buffer, 1);  // NOLINT
}

Compress::Compress(Decompress& dec, size_t memhint)
    : jpeg_compress_struct{},
      dst_{std::make_unique<ManagedMemoryDestination>(memhint)}
{
  jpeg_create_compress(this);

  err = dec.err;
  jpeg_copy_critical_parameters(&dec, this);
  progressive_mode = static_cast<boolean>(FALSE);
  optimize_coding = static_cast<boolean>(TRUE);
  dest = dst_.get();
}

Compress::Compress(Decompress& dec, uint8_t* buffer, size_t capacity)
    : jpeg_compress_struct{},
      dst_{std::make_unique<UnmanagedMemoryDestination>(buffer, capacity)}
{
  jpeg_create_compress(this);

  err = dec.err;
  jpeg_copy_critical_parameters(&dec, this);
  progressive_mode = static_cast<boolean>(FALSE);
  optimize_coding = static_cast<boolean>(TRUE);
  dest = dst_.get();
}

void MemoryDestination::init(j_compress_ptr compress)
{
  const auto dest = reinterpret_cast<Compress*>(compress)->Dest();
  if (dest == nullptr) {
    return;
  }
  return dest->Init();
}

boolean MemoryDestination::empty(j_compress_ptr compress)
{
  const auto dest = reinterpret_cast<Compress*>(compress)->Dest();
  if (dest == nullptr) {
    return static_cast<boolean>(FALSE);
  }
  return dest->Empty();
}

void MemoryDestination::term(j_compress_ptr compress)
{
  const auto dest = reinterpret_cast<Compress*>(compress)->Dest();
  if (dest == nullptr) {
    return;
  }
  return dest->Term();
}

boolean ManagedMemoryDestination::Empty()
{
  size_ = capacity_ - free_in_buffer;
  auto newcap = capacity_ + buffer_growth;
  auto newbuf = reinterpret_cast<uint8_t*>(realloc(buffer_.get(), newcap));
  if (newbuf == nullptr) {
    size_ = capacity_ = 0;
    buffer_.reset();
    next_output_byte = nullptr;
    free_in_buffer = 0;
    return static_cast<boolean>(FALSE);
  }
  (void)buffer_.release();
  buffer_.reset(newbuf);
  capacity_ = newcap;
  next_output_byte =
      reinterpret_cast<decltype(next_output_byte)>(newbuf) + size_;
  free_in_buffer = buffer_growth;
  return static_cast<boolean>(TRUE);
}

void ManagedMemoryDestination::Term()
{
  size_ = capacity_ - free_in_buffer;
  if (free_in_buffer < buffer_growth) {
    return;
  }
  auto newbuf = reinterpret_cast<uint8_t*>(realloc(buffer_.get(), size_));
  if (newbuf == nullptr) {
    return;
  }
  (void)buffer_.release();
  buffer_.reset(newbuf);
  capacity_ = size_;
  free_in_buffer = 0;
}

Optimizer::Optimizer(
    Local<Promise::Resolver>& res,
    Local<ArrayBufferView>& buf,
    MaybeLocal<ArrayBufferView>& outbuf,
    StripFlags flags)
    : Nan::AsyncWorker(nullptr, "jpegoptimize"),
      buffer_{BufferData(buf)},
      len_{buf->ByteLength()},
#ifdef HAS_EXIF
      stripThumb_{(flags & StripThumbnail) == StripThumbnail},
#endif
      stripMeta_{(flags & StripMeta) == StripMeta},
      stripICC_{(flags & StripICC) == StripICC}
{
  SaveToPersistent("buf", buf);
  SaveToPersistent("res", res);
  if (!outbuf.IsEmpty()) {
    auto obuf = outbuf.ToLocalChecked();
    SaveToPersistent("out", obuf);
    outbuf_ = BufferData(obuf);
    outlen_ = obuf->ByteLength();
  }
}

bool Optimizer::CopyMarkers(ErrorManager& err, Decompress& dec)
{
  auto marker = dec.marker_list;
  auto sawICC{false};
  std::vector<decltype(marker)> mrks;
  while (marker != nullptr) {
    switch (marker->marker) {
    case JPEG_APP0 + 1:
      if (stripMeta_) {
        break;
      }
      if (marker->data_length > TAG_EXIF_LEN &&
          memcmp(marker->data, TAG_EXIF, TAG_EXIF_LEN) == 0) {
        mrks.push_back(marker);
      }
      else if (
          marker->data_length > TAG_XMP_LEN &&
          memcmp(marker->data, TAG_XMP, TAG_XMP_LEN) == 0) {
        mrks.push_back(marker);
      }
      break;

    case JPEG_APP0 + 2:
      if (stripICC_) {
        break;
      }
      if (sawICC ||
          (marker->data_length > TAG_ICC_LEN &&
           memcmp(marker->data, TAG_ICC, TAG_ICC_LEN) == 0)) {
        sawICC = true;
        mrks.push_back(marker);
      }
      break;

    case JPEG_APP0 + 13:
      if (stripMeta_) {
        break;
      }
      if (marker->data_length > TAG_IPTC_LEN &&
          memcmp(marker->data, TAG_IPTC, TAG_IPTC_LEN) == 0) {
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
#ifdef HAS_EXIF
    if (m->marker == JPEG_APP0 + 1 && !replacementExif.empty()) {
      jpeg_write_marker(
          compress_.get(), m->marker,
          reinterpret_cast<const uint8_t*>(replacementExif.data()),
          replacementExif.size());
      replacementExif.clear();
    }
    else
#endif
    {
      jpeg_write_marker(compress_.get(), m->marker, m->data, m->data_length);
    }
    if (err) {
      compress_.reset();
      SetErrorMessage(err.msg());
      return false;
    }
  }

  return true;
}

void Optimizer::Execute()
{
  ErrorManager err;
  if (setjmp(err.setjmp_buffer)) {  // NOLINT
    invalid_ = err.invalid();
    if (err) {
      return SetErrorMessage(err.msg());
    }
    return SetErrorMessage("Invalid Image");
  }

#ifdef HAS_EXIF
  if (!stripMeta_ && stripThumb_) {
    std::unique_ptr<ExifData, ed> exif(exif_data_new_from_data(buffer_, len_));
    if (exif && exif->data != nullptr && exif->size > 0) {
      free(exif->data);  // NOLINT
      exif->data = nullptr;
      exif->size = 0;
      exif_data_fix(exif.get());

      unsigned char* data;
      unsigned int len;
      exif_data_save_data(exif.get(), &data, &len);
      std::unique_ptr<char, free_deleter<char>> pdata(
          reinterpret_cast<char*>(data));
      if (pdata && len > 0) {
        replacementExif = std::string(pdata.get(), len);
      }
    }
  }
#endif

  Decompress dec(&err, stripMeta_, stripICC_);
  dec.init(buffer_, len_);
  const auto coefs = jpeg_read_coefficients(&dec);
  if (err) {
    return SetErrorMessage(err.msg());
  }
  if (coefs == nullptr) {
    return SetErrorMessage("Invalid image");
  }

  if (outbuf_ != nullptr) {
    compress_ = std::make_unique<Compress>(dec, outbuf_, outlen_);
  }
  else {
    compress_ = std::make_unique<Compress>(dec, len_);
  }
  compress_->Init(coefs);
  if (err) {
    compress_.reset();
    SetErrorMessage(err.msg());
    return;
  }

  if (!CopyMarkers(err, dec)) {
    return;
  }

  compress_->Finish();
  if (err) {
    compress_.reset();
    SetErrorMessage(err.msg());
    return;
  }
}

void Optimizer::HandleOKCallback()
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
  auto dest = compress_->Buffer();
  compress_.reset();
  auto isolate = Isolate::GetCurrent();

  if (!dest->Managed()) {
    auto length = Nan::New<Number>(dest->Length());
    resolver->Resolve(Nan::GetCurrentContext(), length).IsNothing();
    dest.reset();
    return;
  }

  auto buf = node::Buffer::New(
      isolate, reinterpret_cast<char*>(dest->Data()), dest->Length(),
      MemoryDestination::destroy, nullptr);
  if (buf.IsEmpty()) {
    auto err = Nan::Error("Cannot create output buffer");
    resolver->Reject(Nan::GetCurrentContext(), err).IsNothing();
    return;
  }

  auto lbuf = buf.ToLocalChecked();
  new Holder<MemoryDestination>(isolate, lbuf, std::move(dest));
  resolver->Resolve(Nan::GetCurrentContext(), lbuf).IsNothing();
}

void Optimizer::HandleErrorCallback()
{
  Nan::HandleScope scope;
  GetFromPersistent("buf");
  GetFromPersistent("out");
  compress_.reset();

  auto resolver = GetFromPersistent("res").As<Promise::Resolver>();
  auto err = Nan::Error(ErrorMessage()).As<Object>();
  Nan::DefineOwnProperty(
      err, Nan::New("invalid").ToLocalChecked(), Nan::New(invalid_));
  resolver->Reject(Nan::GetCurrentContext(), err).IsNothing();
}
}  // namespace jpegoptim

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

  const auto flags =
      static_cast<jpegoptim::StripFlags>(Nan::To<uint32_t>(info[1]).FromJust());
#ifndef HAS_EXIF
  if ((flags & jpegoptim::StripThumbnail) == jpegoptim::StripThumbnail) {
    return Nan::ThrowRangeError(
        "node-jpegoptim was compiled without libexif support; cannot stripThumbnail");
  }
#endif

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
  Nan::AsyncQueueWorker(new jpegoptim::Optimizer(resolver, buf, outbuf, flags));
  info.GetReturnValue().Set(promise);
}

#ifdef __GNUC__
#  pragma GCC visibility pop
#endif

NAN_MODULE_INIT(InitAll)
{
  Nan::Set(
      target, Nan::New("_optimize").ToLocalChecked(),
      Nan::GetFunction(Nan::New<FunctionTemplate>(optimize)).ToLocalChecked());

  Local<Object> versions = Nan::New<Object>();
  jpegoptim::ErrorManager err;

  std::string jversion{err.jpeg_message_table[JMSG_VERSION]};
  Nan::Set(
    versions, Nan::New("JPEG_VERSION").ToLocalChecked(), Nan::New(jversion).ToLocalChecked());

  std::string jcopy{err.jpeg_message_table[JMSG_COPYRIGHT]};
  Nan::Set(
    versions, Nan::New("JPEG_COPYRIGHT").ToLocalChecked(), Nan::New(jcopy).ToLocalChecked());

#ifdef HAS_EXIF
  Nan::Set(
    versions, Nan::New("LIBEXIF_VERSION").ToLocalChecked(), Nan::New("Unknown").ToLocalChecked());
#endif

  Nan::Set(target, Nan::New("_versions").ToLocalChecked(), versions);
}

NODE_MODULE(binding, InitAll)
