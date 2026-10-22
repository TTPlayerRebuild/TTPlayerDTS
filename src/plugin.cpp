#include "common.h"
#include "ttpcomm/audio_format_probe_impl.h"
namespace ttp::dts {
HMODULE module{};
StandardContent standard_content() {
  for (const wchar_t *name : {L"ttpctrl.dll", L"soundcore.dll",
                              static_cast<const wchar_t *>(nullptr)})
    if (auto h = GetModuleHandleW(name))
      if (auto p = GetProcAddress(h, "CreateStdContent"))
        return reinterpret_cast<StandardContent>(p);
  return nullptr;
}
class Factory final : public ReaderCreator, public ttp::format::ProbeSupport {
protected:
  DWORD ProbeFamilies() const noexcept override { return ttp::format::Dts; }

private:
  LONG refs_{1};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    if (IsEqualIID(id, ttp::format::iid_probe)) {
      *out = static_cast<ttp::format::Probe *>(this);
      AddRef();
      return S_OK;
    }

    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!same(id, IID_IUnknown) && !same(id, cat_reader))
      return E_NOINTERFACE;
    *out = static_cast<ReaderCreator *>(this);
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ProbeFormat(const ttp::format::Request *q,
                                        ttp::format::Result *out) override {
    if (!q || !out)
      return E_POINTER;
    if (out->size != sizeof(*out) || out->version != ttp::format::abi_version ||
        q->size != sizeof(*q) || q->version != ttp::format::abi_version ||
        q->bytes > ttp::format::prefix_limit || (!q->data && q->bytes))
      return E_INVALIDARG;
    auto result =
        ttp::format::Identify(*q, ttp::format::Dts | ttp::format::Wave);
    if (result.family == ttp::format::Wave && strcmp(result.codec, "dts"))
      result = {};
    if (result.family == ttp::format::Dts &&
        ttp::format::HasDtsFrames({q->data, q->bytes}))
      result.confidence = ttp::format::Strong;
    *out = result;
    return result.confidence >= ttp::format::Tentative ? S_OK : S_FALSE;
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
  HRESULT STDMETHODCALLTYPE Create(void **out) override {
    return make_reader(out);
  }
  HRESULT STDMETHODCALLTYPE Name(wchar_t **out) override {
    return text(L"DTS Reader", out);
  }
  HRESULT STDMETHODCALLTYPE Extensions(wchar_t **out) override {
    return text(L"DTS 音频文件(*.dts;*.dtshd;*.dtswav;*.wav;*.wave)", out);
  }
};
class SoundAddIn final : public AddIn {
  LONG refs_{1};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!same(id, IID_IUnknown) && !same(id, iid_addin))
      return E_NOINTERFACE;
    *out = static_cast<AddIn *>(this);
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
  HRESULT STDMETHODCALLTYPE Enum(DWORD index, GUID *category,
                                 void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!category)
      return E_POINTER;
    if (index)
      return E_INVALIDARG;
    return protect([&]() -> HRESULT {
      *out = static_cast<ReaderCreator *>(new Factory);
      *category = cat_reader;
      return S_OK;
    });
  }
};
} // namespace ttp::dts
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  return ttp::dts::protect([&]() -> HRESULT {
    *out = static_cast<ttp::dts::AddIn *>(new ttp::dts::SoundAddIn);
    return S_OK;
  });
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    ttp::dts::module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
