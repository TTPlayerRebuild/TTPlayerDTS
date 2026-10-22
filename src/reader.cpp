#include "io.h"
#include "ttpcomm/audio_format_probe_impl.h"
#include <atomic>
#include <cmath>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace ttp::dts {
namespace {
constexpr bool dsd_plugin = false;
constexpr GUID pcm_guid{1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID float_guid{3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
bool is_dsd(AVCodecID id) {
  return id == AV_CODEC_ID_DSD_LSBF || id == AV_CODEC_ID_DSD_MSBF ||
         id == AV_CODEC_ID_DSD_LSBF_PLANAR ||
         id == AV_CODEC_ID_DSD_MSBF_PLANAR || id == AV_CODEC_ID_DST;
}
struct Tag {
  std::string key;
  std::wstring value;
};
struct Art {
  Bytes data;
  std::wstring mime, description;
  Picture view{};
};
class AudioReader final : public Reader,
                          public Metadata,
                          public Thumbnail,
                          public ttp::format::SourceInfo {
  LONG refs_{1};
  Input input_;
  AVFormatContext *container_{};
  AVIOContext *io_{};
  AVCodecContext *decoder_{};
  AVPacket *packet_{av_packet_alloc()};
  AVFrame *frame_{av_frame_alloc()};
  SwrContext *converter_{};
  WAVEFORMATEXTENSIBLE wave_{};
  ttp::format::Result source_info_{};
  AVChannelLayout layout_{};
  int stream_index_{-1}, input_rate_{}, input_sample_format_{};
  std::vector<Tag> tags_;
  std::vector<Art> art_;
  Bytes pcm_;
  size_t consumed_{};
  uint64_t position_{}, dsf_samples_{}, total_output_{}, decoded_samples_{};
  uint64_t dsdiff_bytes_{}, expected_input_{};
  DWORD duration_{}, bitrate_{};
  WORD depth_{};
  HRESULT io_error_{S_OK};
  bool opened_{}, draining_{}, decoder_end_{}, eof_{}, floating_{};
  std::atomic_bool stopped_{};
  std::wstring codec_name_;
  static int read_callback(void *opaque, uint8_t *data, int bytes) noexcept {
    auto &s = *static_cast<AudioReader *>(opaque);
    ULONG got{};
    auto hr = protect([&]() -> HRESULT {
      require(bytes > 0, E_INVALIDARG);
      require(!s.stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      got = s.input_.read(data, DWORD(bytes));
      return S_OK;
    });
    if (FAILED(hr)) {
      s.io_error_ = hr;
      return AVERROR(EIO);
    }
    return got ? int(got) : AVERROR_EOF;
  }
  static int64_t seek_callback(void *opaque, int64_t offset,
                               int whence) noexcept {
    auto &s = *static_cast<AudioReader *>(opaque);
    int64_t result = -1;
    if (whence == AVSEEK_SIZE)
      return s.input_.size && s.input_.size >= s.input_.base
                 ? int64_t(s.input_.size - s.input_.base)
                 : AVERROR(ENOSYS);
    auto hr = protect([&]() -> HRESULT {
      require(s.input_.seekable, STG_E_INVALIDFUNCTION);
      const int origin = whence & ~AVSEEK_FORCE;
      require(origin >= SEEK_SET && origin <= SEEK_END, E_INVALIDARG);
      LARGE_INTEGER distance{};
      distance.QuadPart = offset;
      if (origin == SEEK_SET) {
        require(offset >= 0 && uint64_t(offset) <= INT64_MAX - s.input_.base);
        distance.QuadPart += s.input_.base;
      }
      ULARGE_INTEGER pos{};
      check(s.input_.stream->Seek(distance, DWORD(origin), &pos));
      require(pos.QuadPart >= s.input_.base && pos.QuadPart <= INT64_MAX);
      result = int64_t(pos.QuadPart - s.input_.base);
      return S_OK;
    });
    if (FAILED(hr)) {
      s.io_error_ = hr;
      return AVERROR(EIO);
    }
    return result;
  }
  void check_av(int result) {
    check(io_error_);
    if (result < 0)
      throw Failure{result == AVERROR(ENOMEM)
                        ? E_OUTOFMEMORY
                        : HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
  }
  void close_container() {
    swr_free(&converter_);
    av_channel_layout_uninit(&layout_);
    avcodec_free_context(&decoder_);
    if (packet_) av_packet_unref(packet_);
    if (frame_) av_frame_unref(frame_);
    avformat_close_input(&container_);
    if (io_) {
      av_freep(&io_->buffer);
      avio_context_free(&io_);
    }
  }
  bool decoded_frame() {
    if (decoder_end_)
      return false;
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      int result = avcodec_receive_frame(decoder_, frame_);
      if (result == 0) {
        decoded_samples_ += frame_->nb_samples;
        return true;
      }
      if (result == AVERROR_EOF) {
        if (expected_input_)
          require(decoded_samples_ == expected_input_, STG_E_READFAULT);
        decoder_end_ = true;
        return false;
      }
      if (result != AVERROR(EAGAIN))
        check_av(result);
      require(!draining_);
      do {
        av_packet_unref(packet_);
        result = av_read_frame(container_, packet_);
        check(io_error_);
      } while (result >= 0 && packet_->stream_index != stream_index_);
      if (result == AVERROR_EOF) {
        draining_ = true;
        check_av(avcodec_send_packet(decoder_, nullptr));
      } else {
        check_av(result);
        check_av(avcodec_send_packet(decoder_, packet_));
      }
    }
    throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
  }
  void convert(bool flush = false) {
    if (!flush) {
      require(frame_->nb_samples > 0 && frame_->nb_samples <= 1048576);
      require(frame_->sample_rate == input_rate_ &&
                  frame_->format == input_sample_format_ &&
                  !av_channel_layout_compare(&frame_->ch_layout, &layout_),
              E_NOTIMPL);
    }
    const int count =
        swr_get_out_samples(converter_, flush ? 0 : frame_->nb_samples);
    check_av(count);
    require(count <= 2097152);
    std::vector<double> samples(size_t(std::max(count, 1)) *
                                wave_.Format.nChannels);
    uint8_t *destination = reinterpret_cast<uint8_t *>(samples.data());
    const int frames = swr_convert(
        converter_, &destination, count,
        flush ? nullptr : const_cast<const uint8_t **>(frame_->extended_data),
        flush ? 0 : frame_->nb_samples);
    check_av(frames);
    size_t kept = size_t(frames);
    if (total_output_)
      kept = size_t(std::min<uint64_t>(
          kept, total_output_ > position_ ? total_output_ - position_ : 0));
    pcm_.resize(kept * wave_.Format.nBlockAlign);
    consumed_ = 0;
    for (size_t i = 0; i < kept * wave_.Format.nChannels; ++i) {
      const double value = samples[i];
      require(std::isfinite(value));
      if (floating_)
        memcpy(pcm_.data() + i * 8, &value, 8);
      else {
        const int32_t sample = int32_t(std::llround(
            std::clamp(value, -1.0, 8388607.0 / 8388608.0) * 8388608.0));
        for (unsigned b = 0; b < 3; ++b)
          pcm_[i * 3 + b] = BYTE(uint32_t(sample) >> (8 * b));
      }
    }
    if (!flush)
      av_frame_unref(frame_);
  }
  bool next() {
    while (!eof_) {
      if (decoded_frame())
        convert();
      else {
        convert(true);
        if (pcm_.empty()) {
          if (total_output_)
            require(position_ >= total_output_, STG_E_READFAULT);
          eof_ = true;
        }
      }
      if (!pcm_.empty())
        return true;
    }
    return false;
  }
  void identify_source() {
    using namespace ttp::format;
    source_info_ = {};
    source_info_.confidence = Opened;
    const std::string name = container_->iformat->name;
    const bool wave = name == "wav", dsf = name == "dsf";
    source_info_.family = dsd_plugin ? Dsd : wave ? Wave : Dts;
    Copy(source_info_.container,
         dsd_plugin ? (dsf ? "dsf" : "dsdiff") : (wave ? "wave" : "dts"));
    Copy(source_info_.codec,
         dsd_plugin ? (decoder_->codec_id == AV_CODEC_ID_DST ? "dst" : "dsd")
                    : "dts");
    Copy(source_info_.extension,
         dsd_plugin ? (dsf ? L".dsf" : L".dff") : (wave ? L".wav" : L".dts"));
    Copy(source_info_.aliases,
         dsd_plugin ? (dsf ? L"" : L".dsdiff")
                    : (wave ? L".wave;.bwf;.dtswav" : L".dtshd"));
    Copy(source_info_.display, dsd_plugin ? (dsf ? L"DSF" : L"DSDIFF")
                                          : (wave ? L"DTS-WAV" : L"DTS"));
  }
  void open_container() {
    decoded_samples_ = 0;
    io_error_ = S_OK;
    draining_ = decoder_end_ = eof_ = false;
    pcm_.clear();
    consumed_ = 0;
    auto *buffer = static_cast<unsigned char *>(av_malloc(32768));
    require(buffer, E_OUTOFMEMORY);
    io_ = avio_alloc_context(buffer, 32768, 0, this, read_callback, nullptr,
                             input_.seekable ? seek_callback : nullptr);
    if (!io_) {
      av_free(buffer);
      throw Failure{E_OUTOFMEMORY};
    }
    io_->seekable = input_.seekable ? AVIO_SEEKABLE_NORMAL : 0;
    container_ = avformat_alloc_context();
    require(container_, E_OUTOFMEMORY);
    container_->pb = io_;
    container_->flags |= AVFMT_FLAG_CUSTOM_IO;
    container_->probesize = 1024 * 1024;
    container_->max_analyze_duration = 2 * AV_TIME_BASE;
    check_av(avformat_open_input(&container_, nullptr, nullptr, nullptr));
    check_av(avformat_find_stream_info(container_, nullptr));
    stream_index_ = -1;
    for (unsigned i = 0; i < container_->nb_streams; ++i) {
      const auto *p = container_->streams[i]->codecpar;
      if (p->codec_type == AVMEDIA_TYPE_AUDIO &&
          (dsd_plugin ? is_dsd(p->codec_id) : p->codec_id == AV_CODEC_ID_DTS)) {
        stream_index_ = int(i);
        break;
      }
    }
    require(stream_index_ >= 0, E_NOTIMPL);
    auto *st = container_->streams[stream_index_];
    const auto *p = st->codecpar;
    const auto *codec = avcodec_find_decoder(p->codec_id);
    require(codec, E_NOTIMPL);
    decoder_ = avcodec_alloc_context3(codec);
    require(decoder_, E_OUTOFMEMORY);
    check_av(avcodec_parameters_to_context(decoder_, p));
    decoder_->thread_count = 1;
    decoder_->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
    check_av(avcodec_open2(decoder_, codec, nullptr));
    require(decoded_frame());
    input_rate_ = frame_->sample_rate;
    input_sample_format_ = frame_->format;
    require(input_rate_ > 0 && input_rate_ <= 2822400 &&
            frame_->ch_layout.nb_channels > 0 &&
            frame_->ch_layout.nb_channels <= 8);
    require(frame_->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
                frame_->ch_layout.u.mask <= MAXDWORD,
            E_NOTIMPL);
    check_av(av_channel_layout_copy(&layout_, &frame_->ch_layout));
    const DWORD rate = dsd_plugin
                           ? DWORD(input_rate_ % 44100 == 0 ? 176400 : 192000)
                           : DWORD(input_rate_);
    wave_ = {};
    auto &f = wave_.Format;
    f.nChannels = WORD(layout_.nb_channels);
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = floating_ ? 64 : 24;
    f.nBlockAlign = WORD(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    f.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.cbSize = 22;
    wave_.Samples.wValidBitsPerSample = f.wBitsPerSample;
    wave_.dwChannelMask = DWORD(layout_.u.mask);
    wave_.SubFormat = floating_ ? float_guid : pcm_guid;
    check_av(swr_alloc_set_opts2(
        &converter_, &layout_, AV_SAMPLE_FMT_DBL, int(rate), &layout_,
        AVSampleFormat(input_sample_format_), input_rate_, 0, nullptr));
    // FIR anti-alias filtering precedes the host EQ and output conversion.
    check_av(av_opt_set_int(converter_, "filter_size", 64, 0));
    check_av(swr_init(converter_));
    depth_ = dsd_plugin
                 ? 1
                 : WORD(p->bits_per_raw_sample ? p->bits_per_raw_sample : 24);
    bitrate_ = dword(uint64_t(std::max<int64_t>(0, p->bit_rate)));
    int64_t duration =
        st->duration != AV_NOPTS_VALUE
            ? av_rescale_q(st->duration, st->time_base, {1, 1000})
            : container_->duration / 1000;
    expected_input_ = dsf_samples_ / 8;
    if (dsdiff_bytes_) {
      require(dsdiff_bytes_ % f.nChannels == 0);
      expected_input_ = dsdiff_bytes_ / f.nChannels;
    }
    if (expected_input_) {
      require(expected_input_ <= INT64_MAX / 1000);
      total_output_ = uint64_t(av_rescale_rnd(int64_t(expected_input_), rate,
                                              input_rate_, AV_ROUND_NEAR_INF));
      duration = int64_t(expected_input_) * 1000 / input_rate_;
    }
    duration_ = duration > 0 ? dword(uint64_t(duration)) : 0;
    if (!bitrate_ && duration_ && input_.size <= UINT64_MAX / 8000)
      bitrate_ = dword(input_.size * 8000 / duration_);
    codec_name_ = dsd_plugin ? (p->codec_id == AV_CODEC_ID_DST
                                    ? L"DST|DSDIFF (DSD to PCM)"
                                    : L"DSD|Direct Stream Digital (PCM)")
                             : L"DTS|Digital Theater Systems";
    identify_source();
    convert();
  }
  void read_metadata() {
    const auto add = [&](AVDictionary *dictionary) {
      AVDictionaryEntry *t = nullptr;
      while ((t = av_dict_get(dictionary, "", t, AV_DICT_IGNORE_SUFFIX)))
        tags_.push_back({t->key, wide(t->value)});
    };
    add(container_->metadata);
    add(container_->streams[stream_index_]->metadata);
    for (unsigned i = 0; i < container_->nb_streams; ++i) {
      const auto *stream = container_->streams[i];
      const auto &p = stream->attached_pic;
      if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || p.size <= 0 ||
          p.size > 16 * 1024 * 1024 || art_.size() >= 32)
        continue;
      std::wstring mime;
      if (stream->codecpar->codec_id == AV_CODEC_ID_PNG)
        mime = L"image/png";
      else if (stream->codecpar->codec_id == AV_CODEC_ID_MJPEG)
        mime = L"image/jpeg";
      else if (stream->codecpar->codec_id == AV_CODEC_ID_WEBP)
        mime = L"image/webp";
      else
        continue;
      art_.push_back({Bytes(p.data, p.data + p.size), std::move(mime), {}, {}});
      if (auto *t = av_dict_get(stream->metadata, "title", nullptr, 0))
        art_.back().description = wide(t->value);
    }
    for (auto &p : art_)
      p.view = {sizeof(Picture),      p.mime.c_str(), p.description.c_str(),
                DWORD(p.data.size()), p.data.data(),  3};
  }
  ~AudioReader() {
    close_container();
    av_packet_free(&packet_);
    av_frame_free(&frame_);
  }

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (same(id, IID_IUnknown) || same(id, iid_reader))
      *out = static_cast<Reader *>(this);
    else if (same(id, iid_metadata))
      *out = static_cast<Metadata *>(this);
    else if (same(id, iid_thumbnail))
      *out = static_cast<Thumbnail *>(this);
    else if (same(id, ttp::format::iid_info))
      *out = static_cast<ttp::format::SourceInfo *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = InterlockedDecrement(&refs_);
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Open(IStream *stream, DWORD flags) override {
    if (!stream)
      return E_POINTER;
    if (input_.stream)
      return E_UNEXPECTED;
    return protect([&]() -> HRESULT {
      require(packet_ && frame_, E_OUTOFMEMORY);
      floating_ = (flags & 2) != 0;
      input_.open(stream);
      if (dsd_plugin && input_.seekable) {
        BYTE header[80]{};
        ULONG n = 0;
        while (n < sizeof(header)) {
          const ULONG got = input_.read(header + n, DWORD(sizeof(header) - n));
          if (!got)
            break;
          n += got;
        }
        input_.at(input_.base);
        if (n == sizeof(header) && !memcmp(header, "DSD ", 4)) {
          dsf_samples_ =
              uint64_t(le32(header + 64)) | (uint64_t(le32(header + 68)) << 32);
          require(dsf_samples_ && dsf_samples_ <= INT64_MAX / 1000);
          const uint64_t declared =
              uint64_t(le32(header + 12)) | (uint64_t(le32(header + 16)) << 32);
          if (input_.size)
            require(declared <= input_.size - input_.base, STG_E_READFAULT);
        } else if (n >= 16 && !memcmp(header, "FRM8", 4)) {
          const uint64_t declared = be64(header + 4);
          require(declared <= INT64_MAX - 12);
          if (input_.size)
            require(declared + 12 <= input_.size - input_.base,
                    STG_E_READFAULT);
          const uint64_t end = declared + 12;
          uint64_t at = 16;
          for (unsigned chunks = 0; at <= end && end - at >= 12; ++chunks) {
            require(chunks < 4096);
            input_.at(input_.base + at);
            BYTE chunk[12]{};
            input_.exact(chunk, sizeof(chunk));
            const uint64_t bytes = be64(chunk + 4);
            require(bytes <= end - at - 12, STG_E_READFAULT);
            if (!memcmp(chunk, "DSD ", 4)) {
              dsdiff_bytes_ = bytes;
              break;
            }
            if (!memcmp(chunk, "DST ", 4))
              break;
            at += 12 + bytes + (bytes & 1);
          }
          input_.at(input_.base);
        }
      }
      open_container();
      read_metadata();
      opened_ = true;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Capabilities(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = (input_.remote ? 1u : 0u) | (input_.seekable ? 2u : 0u);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Duration(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = duration_;
    return opened_ ? S_OK : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!opened_)
      return E_UNEXPECTED;
    auto *p = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(sizeof(wave_)));
    if (!p)
      return E_OUTOFMEMORY;
    memcpy(p, &wave_, sizeof(wave_));
    *out = p;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE BufferSize(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = (wave_.Format.nSamplesPerSec / 4) * wave_.Format.nBlockAlign;
    return opened_ ? S_OK : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE CodecName(wchar_t **out) override {
    return text(codec_name_, out);
  }
  HRESULT STDMETHODCALLTYPE Bitrate(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = bitrate_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE BitDepth(WORD *out) override {
    if (!out)
      return E_POINTER;
    *out = depth_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCallback(IUnknown *p) override {
    return input_.stream ? input_.callback(p) : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE Start() override {
    stopped_ = false;
    ResetEvent(input_.event);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override {
    stopped_ = true;
    SetEvent(input_.event);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Read(Buffer *buffer) override {
    if (!buffer)
      return E_POINTER;
    return protect([&]() -> HRESULT {
      check(buffer->SetLength(0));
      require(opened_, E_UNEXPECTED);
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      DWORD capacity{}, unused{};
      BYTE *out{};
      check(buffer->Capacity(&capacity));
      check(buffer->Data(&out, &unused));
      capacity -= capacity % wave_.Format.nBlockAlign;
      require(capacity, E_INVALIDARG);
      require(out, E_POINTER);
      if (consumed_ == pcm_.size() && !next())
        return S_FALSE;
      auto n = DWORD(std::min<size_t>(capacity, pcm_.size() - consumed_));
      memcpy(out, pcm_.data() + consumed_, n);
      consumed_ += n;
      position_ += n / wave_.Format.nBlockAlign;
      return buffer->SetLength(n);
    });
  }
  HRESULT STDMETHODCALLTYPE Seek(DWORD *ms) override {
    if (!ms)
      return E_POINTER;
    return protect([&]() -> HRESULT {
      require(opened_, E_UNEXPECTED);
      require(input_.seekable, STG_E_INVALIDFUNCTION);
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      const uint64_t desired =
          uint64_t(*ms) * wave_.Format.nSamplesPerSec / 1000;
      // Backward seeks replay history to preserve decoder overlap/FIR phase.
      if (desired < position_) {
        close_container();
        input_.at(input_.base);
        position_ = 0;
        open_container();
      }
      while (position_ < desired) {
        if (consumed_ == pcm_.size() && !next())
          break;
        auto n = std::min<uint64_t>(desired - position_,
                                    (pcm_.size() - consumed_) /
                                        wave_.Format.nBlockAlign);
        consumed_ += size_t(n) * wave_.Format.nBlockAlign;
        position_ += n;
      }
      *ms = milliseconds(position_, wave_.Format.nSamplesPerSec);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE GetFormatInfo(ttp::format::Result *out) override {
    if (!out)
      return E_POINTER;
    if (out->size != sizeof(*out) || out->version != ttp::format::abi_version)
      return E_INVALIDARG;
    *out = {};
    if (!opened_)
      return E_UNEXPECTED;
    *out = source_info_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Count(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = DWORD(tags_.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE At(DWORD n, wchar_t **key,
                               wchar_t **value) override {
    if (!key || !value)
      return E_POINTER;
    *key = *value = nullptr;
    if (n >= tags_.size())
      return E_INVALIDARG;
    return protect([&]() -> HRESULT {
      check(text(wide(tags_[n].key), key));
      auto hr = text(tags_[n].value, value);
      if (FAILED(hr)) {
        CoTaskMemFree(*key);
        *key = nullptr;
      }
      return hr;
    });
  }
  HRESULT STDMETHODCALLTYPE Get(const char *key, wchar_t **out) override {
    if (!key || !out)
      return E_POINTER;
    *out = nullptr;
    for (const auto &t : tags_)
      if (!_stricmp(key, t.key.c_str()))
        return text(t.value, out);
    return E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE Set(const char *, const wchar_t *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE PictureCount(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = DWORD(art_.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MaximumCount(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PictureAt(DWORD n, const Picture **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (n >= art_.size())
      return E_INVALIDARG;
    *out = &art_[n].view;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD, const Picture *,
                                           DWORD) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE AddPicture(const Picture *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE RemovePicture(DWORD) override { return E_NOTIMPL; }
};
} // namespace
HRESULT make_reader(void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  return protect([&]() -> HRESULT {
    *out = static_cast<Reader *>(new AudioReader);
    return S_OK;
  });
}
} // namespace ttp::dts
