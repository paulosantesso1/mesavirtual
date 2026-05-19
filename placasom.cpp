#include <windows.h>
#include <string>
#include <iostream>
#include <vector>
#include <set>
#include <atomic>
#include <thread>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <Functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

#define REFTIMES_PER_SEC 10000000
const GUID GUID_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// ---------------------------------------------------------------------------
// ResolverPidAudio — encontra o PID que TEM SESSAO DE AUDIO ATIVA
// para o conjunto de PIDs fornecido, usando IAudioSessionManager2.
// Isso garante que capturamos o processo certo sem capturar arvores inteiras.
// Se nenhum PID tiver sessao ativa no momento, retorna o menor PID do grupo.
// ---------------------------------------------------------------------------
DWORD ResolverPidAudio(const std::vector<DWORD>& pids) {
    if (pids.empty()) return 0;
    if (pids.size() == 1) return pids[0];

    // Cria um set para busca rapida
    std::set<DWORD> pidSet(pids.begin(), pids.end());
    DWORD resultado = *std::min_element(pids.begin(), pids.end());

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice*           pDev  = nullptr;
    IAudioSessionManager2* pMgr = nullptr;
    IAudioSessionEnumerator* pSessEnum = nullptr;

    auto Cleanup = [&]() {
        if (pSessEnum) pSessEnum->Release();
        if (pMgr)      pMgr->Release();
        if (pDev)      pDev->Release();
        if (pEnum)     pEnum->Release();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
        return resultado;

    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
        { Cleanup(); return resultado; }

    if (FAILED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                               NULL, (void**)&pMgr)))
        { Cleanup(); return resultado; }

    if (FAILED(pMgr->GetSessionEnumerator(&pSessEnum)))
        { Cleanup(); return resultado; }

    int count = 0;
    pSessEnum->GetCount(&count);

    for (int i = 0; i < count; i++) {
        IAudioSessionControl*  pCtrl  = nullptr;
        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pSessEnum->GetSession(i, &pCtrl))) continue;
        if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                            (void**)&pCtrl2))) {
            DWORD pid = 0;
            AudioSessionState state;
            if (SUCCEEDED(pCtrl2->GetProcessId(&pid)) &&
                SUCCEEDED(pCtrl->GetState(&state)) &&
                state == AudioSessionStateActive &&
                pidSet.count(pid)) {
                resultado = pid;
                pCtrl2->Release();
                pCtrl->Release();
                break;
            }
            pCtrl2->Release();
        }
        pCtrl->Release();
    }

    Cleanup();
    return resultado;
}

// ---------------------------------------------------------------------------
// LockFreeRingBuffer — SPSC
// ---------------------------------------------------------------------------
template <typename T>
class LockFreeRingBuffer {
private:
    std::vector<T>      buffer;
    const size_t        capacity;
    std::atomic<size_t> write_index;
    std::atomic<size_t> read_index;
public:
    LockFreeRingBuffer(size_t size) : capacity(size + 1), buffer(size + 1) {
        write_index.store(0, std::memory_order_relaxed);
        read_index.store(0,  std::memory_order_relaxed);
    }
    size_t GetAvailableRead() const {
        size_t w = write_index.load(std::memory_order_acquire);
        size_t r = read_index.load(std::memory_order_relaxed);
        return (w >= r) ? w - r : capacity - r + w;
    }
    size_t GetAvailableWrite() const {
        size_t w = write_index.load(std::memory_order_relaxed);
        size_t r = read_index.load(std::memory_order_acquire);
        return (w >= r) ? capacity - 1 - (w - r) : r - w - 1;
    }
    bool Push(const T* data, size_t count) {
        if (GetAvailableWrite() < count) return false;
        size_t cur = write_index.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            buffer[cur] = data[i];
            cur = (cur + 1) % capacity;
        }
        write_index.store(cur, std::memory_order_release);
        return true;
    }
    bool Pop(T* dest, size_t count) {
        if (GetAvailableRead() < count) return false;
        size_t cur = read_index.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            dest[i] = buffer[cur];
            cur = (cur + 1) % capacity;
        }
        read_index.store(cur, std::memory_order_release);
        return true;
    }
};

// ---------------------------------------------------------------------------
// CAsyncAudioActivator — IAgileObject obrigatorio para Process Loopback
// ---------------------------------------------------------------------------
class CAsyncAudioActivator : public IActivateAudioInterfaceCompletionHandler,
                             public IAgileObject {
private:
    std::atomic<ULONG> m_refCount;
public:
    HANDLE        m_hEvent;
    IAudioClient* m_pAudioClient;
    HRESULT       m_hr;

    CAsyncAudioActivator()
        : m_refCount(1),
          m_hEvent(CreateEvent(NULL, FALSE, FALSE, NULL)),
          m_pAudioClient(nullptr), m_hr(S_OK) {}

    ~CAsyncAudioActivator() {
        if (m_hEvent)       CloseHandle(m_hEvent);
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG val = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (val == 0) delete this;
        return val;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* pOp) override
    {
        HRESULT hrAct = E_FAIL; IUnknown* pUnk = nullptr;
        m_hr = pOp->GetActivateResult(&hrAct, &pUnk);
        if (SUCCEEDED(m_hr) && SUCCEEDED(hrAct) && pUnk) {
            pUnk->QueryInterface(__uuidof(IAudioClient), (void**)&m_pAudioClient);
            pUnk->Release();
        } else { m_hr = FAILED(hrAct) ? hrAct : m_hr; }
        if (m_hEvent) SetEvent(m_hEvent);
        return S_OK;
    }
};

struct AudioNode {
    IAudioClient*       client     = nullptr;
    IAudioRenderClient* render     = nullptr;
    WAVEFORMATEX*       format     = nullptr;
    HANDLE              hEvent     = NULL;
    UINT32              bufferSize = 0;
};

class AudioEngine {
private:
    // -----------------------------------------------------------------------
    // Volumes atomicos — atualizados em tempo real pelo main thread (C#)
    // sem necessidade de reiniciar o motor.
    // volMic:  0.0 = silenciado, 1.0 = 100%, 2.0 = 200%
    // volProc: idem para os processos capturados
    // micAtivo: false = microfone desativado completamente
    // -----------------------------------------------------------------------
    std::atomic<float> volMic;
    std::atomic<float> volProc;
    std::atomic<bool>  micAtivo;

    LockFreeRingBuffer<float>*              bufferMic     = nullptr;
    std::vector<LockFreeRingBuffer<float>*> processBuffers;
    std::atomic<bool>                       isRunning;
    std::thread                             threadMic;
    std::vector<std::thread>                processThreads;
    std::thread                             threadOutput;

    bool IsFormatFloat(WAVEFORMATEX* pwfx) {
        if (!pwfx) return false;
        if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* pEx = (WAVEFORMATEXTENSIBLE*)pwfx;
            if (memcmp(&pEx->SubFormat, &GUID_IEEE_FLOAT, sizeof(GUID)) == 0) return true;
        }
        return false;
    }

    float LeAmostra(BYTE* d, UINT32 fi, UINT32 ch, UINT32 ci, WORD bits, bool isFloat) {
        if (!d || ci >= ch) return 0.0f;
        if (isFloat)    return ((float*)d)[fi * ch + ci];
        if (bits == 16) return ((int16_t*)d)[fi * ch + ci] / 32768.0f;
        if (bits == 32) return ((int32_t*)d)[fi * ch + ci] / 2147483648.0f;
        return 0.0f;
    }

    void EscreveAmostra(BYTE* d, UINT32 fi, UINT32 ch, UINT32 ci,
                        WORD bits, bool isFloat, float s) {
        if (!d || ci >= ch) return;
        if (isFloat)         ((float*)d)[fi*ch+ci]   = s;
        else if (bits == 16) ((int16_t*)d)[fi*ch+ci] = (int16_t)(s * 32767.0f);
        else if (bits == 32) ((int32_t*)d)[fi*ch+ci] = (int32_t)(s * 2147483647.0f);
    }

    float Clamp(float s) { return s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s); }

    IMMDevice* GetDeviceByIndex(EDataFlow flow, int index) {
        if (index < 0) return nullptr;
        IMMDeviceEnumerator* pEnum = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
            return nullptr;
        IMMDeviceCollection* pCol = nullptr;
        pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol);
        IMMDevice* pDevice = nullptr;
        HRESULT hr = E_FAIL;
        if (pCol) { hr = pCol->Item(index, &pDevice); pCol->Release(); }
        pEnum->Release();
        return SUCCEEDED(hr) ? pDevice : nullptr;
    }

    // -----------------------------------------------------------------------
    // InitializeWithMinLatency — tenta buffers menores com fallback automatico.
    // -----------------------------------------------------------------------
    HRESULT InitializeWithMinLatency(IAudioClient* pAC, DWORD flags,
                                     WAVEFORMATEX* pwfx, REFERENCE_TIME* outDur) {
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        pAC->GetDevicePeriod(&defPeriod, &minPeriod);

        REFERENCE_TIME candidates[] = { 10000, 30000, 50000, 100000 };
        for (REFERENCE_TIME dur : candidates) {
            REFERENCE_TIME tryDur = max(dur, minPeriod);
            HRESULT hr = pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         flags, tryDur, 0, pwfx, NULL);
            if (SUCCEEDED(hr)) {
                if (outDur) *outDur = tryDur;
                std::cout << "  -> Buffer: " << (tryDur/10000) << "ms\n";
                return hr;
            }
        }
        HRESULT hr = pAC->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, pwfx, NULL);
        if (SUCCEEDED(hr) && outDur) *outDur = defPeriod;
        return hr;
    }

    // -----------------------------------------------------------------------
    // CaptureMicLoop — captura o microfone fisico.
    // Se micIdx == -1 (desativado), apenas empurra silencio no buffer.
    // -----------------------------------------------------------------------
    void CaptureMicLoop(int deviceIndex) {
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;

        // Mic desativado: aloca buffer com taxa padrao e nao captura nada.
        // O MixerLoop vai ler silencio (Pop retorna false → 0.0f).
        if (deviceIndex < 0) {
            bufferMic = new LockFreeRingBuffer<float>(48000 * 2 * 2);
            while (isRunning) Sleep(100);
            CoUninitialize();
            return;
        }

        IMMDevice* pDevice = GetDeviceByIndex(eCapture, deviceIndex);
        if (!pDevice) {
            bufferMic = new LockFreeRingBuffer<float>(48000 * 2 * 2);
            while (isRunning) Sleep(100);
            CoUninitialize(); return;
        }

        IAudioClient*        pAC  = nullptr;
        IAudioCaptureClient* pCC  = nullptr;
        WAVEFORMATEX*        pwfx = nullptr;
        HANDLE               hEv  = NULL;

        auto Cleanup = [&]() {
            if (pAC)  { pAC->Stop(); pAC->Release(); }
            if (pCC)    pCC->Release();
            if (pwfx)   CoTaskMemFree(pwfx);
            if (hEv)    CloseHandle(hEv);
            pDevice->Release();
            CoUninitialize();
        };

        if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     NULL, (void**)&pAC)))
            { bufferMic = new LockFreeRingBuffer<float>(48000*2*2); Cleanup(); return; }

        pAC->GetMixFormat(&pwfx);
        bool   isFloat    = IsFormatFloat(pwfx);
        WORD   bits       = pwfx->wBitsPerSample;
        UINT32 ch         = pwfx->nChannels;
        UINT32 sampleRate = pwfx->nSamplesPerSec;

        std::cout << "[Mic] Taxa: " << sampleRate << " Hz, "
                  << ch << " canal(is), " << bits << " bits\n";

        // Buffer proporcional à taxa real do microfone (2 segundos)
        bufferMic = new LockFreeRingBuffer<float>(sampleRate * 2 * 2);

        REFERENCE_TIME dur = 0;
        if (FAILED(InitializeWithMinLatency(pAC, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                            pwfx, &dur)))
            { delete bufferMic; bufferMic = nullptr; Cleanup(); return; }

        hEv = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!hEv) { delete bufferMic; bufferMic = nullptr; Cleanup(); return; }

        pAC->SetEventHandle(hEv);
        pAC->GetService(__uuidof(IAudioCaptureClient), (void**)&pCC);
        pAC->Start();

        std::vector<float> tmp;
        while (isRunning) {
            if (WaitForSingleObject(hEv, 2000) == WAIT_TIMEOUT) continue;
            UINT32 pktLen = 0;
            pCC->GetNextPacketSize(&pktLen);
            while (pktLen != 0) {
                BYTE* pData; UINT32 nFrames; DWORD flags;
                if (SUCCEEDED(pCC->GetBuffer(&pData, &nFrames, &flags, NULL, NULL))) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && micAtivo.load()) {
                        float vMic = volMic.load();
                        tmp.resize(nFrames * 2);
                        for (UINT32 i = 0; i < nFrames; ++i) {
                            float L = LeAmostra(pData, i, ch, 0, bits, isFloat) * vMic;
                            float R = (ch > 1)
                                ? LeAmostra(pData, i, ch, 1, bits, isFloat) * vMic
                                : L;
                            tmp[i*2] = L; tmp[i*2+1] = R;
                        }
                        if (bufferMic) bufferMic->Push(tmp.data(), nFrames * 2);
                    }
                    pCC->ReleaseBuffer(nFrames);
                }
                pCC->GetNextPacketSize(&pktLen);
            }
        }
        Cleanup();
    }

    // -----------------------------------------------------------------------
    // Thread de ativacao do Process Loopback (STA limpo via CreateThread)
    // -----------------------------------------------------------------------
    struct ActivationContext {
        DWORD         targetPid;
        IAudioClient* pAudioClient;
        HANDLE        hDone;
    };

    static DWORD WINAPI ActivationThreadProc(LPVOID lpParam) {
        auto* ctx = (ActivationContext*)lpParam;
        ctx->pAudioClient = nullptr;

        if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
            { SetEvent(ctx->hDone); return 1; }

        // Janela oculta para garantir message pump no STA
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"PlacasomActivationWnd";
        RegisterClassExW(&wc);
        HWND hWnd = CreateWindowExW(0, L"PlacasomActivationWnd", L"", 0,
                                    0, 0, 0, 0, HWND_MESSAGE, NULL,
                                    GetModuleHandleW(NULL), NULL);

        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        // EXCLUDE: captura apenas o processo especifico, sem filhos.
        // Evita capturar processos filhos como TeamTalk quando o pai e o NVDA.
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        params.ProcessLoopbackParams.TargetProcessId = ctx->targetPid;

        PROPVARIANT propvar;
        PropVariantInit(&propvar);
        propvar.vt             = VT_BLOB;
        propvar.blob.cbSize    = sizeof(params);
        propvar.blob.pBlobData = (BYTE*)&params;

        CAsyncAudioActivator* pActivator = new CAsyncAudioActivator();
        if (!pActivator->m_hEvent)
            { pActivator->Release(); CoUninitialize(); SetEvent(ctx->hDone); return 1; }
        pActivator->AddRef();

        IActivateAudioInterfaceAsyncOperation* pOp = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient), &propvar, pActivator, &pOp);

        if (FAILED(hr)) {
            std::cerr << "[Activate PID=" << ctx->targetPid << "] falhou: 0x"
                      << std::hex << hr << std::dec << "\n";
            pActivator->Release(); pActivator->Release();
            if (hWnd) DestroyWindow(hWnd);
            UnregisterClassW(L"PlacasomActivationWnd", GetModuleHandleW(NULL));
            CoUninitialize(); SetEvent(ctx->hDone); return 1;
        }

        while (true) {
            DWORD r = MsgWaitForMultipleObjects(
                1, &pActivator->m_hEvent, FALSE, 5000, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                continue;
            }
            std::cerr << "[Activate PID=" << ctx->targetPid << "] timeout\n";
            break;
        }

        if (FAILED(pActivator->m_hr)) {
            std::cerr << "[Activate PID=" << ctx->targetPid << "] erro: 0x"
                      << std::hex << pActivator->m_hr << std::dec << "\n";
        } else if (pActivator->m_pAudioClient) {
            ctx->pAudioClient = pActivator->m_pAudioClient;
            ctx->pAudioClient->AddRef();
        }

        pActivator->Release(); pOp->Release();
        if (hWnd) DestroyWindow(hWnd);
        UnregisterClassW(L"PlacasomActivationWnd", GetModuleHandleW(NULL));
        CoUninitialize(); SetEvent(ctx->hDone); return 0;
    }

    IAudioClient* ActivateProcessClient(DWORD targetPid) {
        ActivationContext ctx { targetPid, nullptr,
            CreateEvent(NULL, FALSE, FALSE, NULL) };
        if (!ctx.hDone) return nullptr;

        HANDLE hThread = CreateThread(NULL, 0, ActivationThreadProc, &ctx, 0, NULL);
        if (!hThread) { CloseHandle(ctx.hDone); return nullptr; }

        WaitForSingleObject(ctx.hDone, 8000);
        WaitForSingleObject(hThread, 8000);
        CloseHandle(hThread); CloseHandle(ctx.hDone);

        if (ctx.pAudioClient)
            std::cout << "[Activate PID=" << targetPid << "] Sucesso.\n";
        else
            std::cerr << "[Activate PID=" << targetPid << "] Falhou.\n";

        return ctx.pAudioClient;
    }

    // -----------------------------------------------------------------------
    // CaptureProcessLoop — captura o audio de um processo especifico.
    // Aplica volProc em tempo real.
    // -----------------------------------------------------------------------
    void CaptureProcessLoop(DWORD targetPid, IAudioClient* pAC,
                            LockFreeRingBuffer<float>* targetBuffer,
                            UINT32 outputSampleRate) {
        if (!pAC) return;
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
            { pAC->Release(); return; }

        WAVEFORMATEX captureFormat = {};
        captureFormat.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        captureFormat.nChannels       = 2;
        captureFormat.nSamplesPerSec  = outputSampleRate;
        captureFormat.wBitsPerSample  = 32;
        captureFormat.nBlockAlign     = captureFormat.nChannels *
                                        captureFormat.wBitsPerSample / 8;
        captureFormat.nAvgBytesPerSec = captureFormat.nSamplesPerSec *
                                        captureFormat.nBlockAlign;
        captureFormat.cbSize = 0;

        IAudioCaptureClient* pCC = nullptr;
        HANDLE               hEv = NULL;

        auto Cleanup = [&]() {
            if (pAC)  { pAC->Stop(); pAC->Release(); }
            if (pCC)    pCC->Release();
            if (hEv)    CloseHandle(hEv);
            CoUninitialize();
        };

        std::cout << "[ProcessCapture PID=" << targetPid << "] Formato: "
                  << outputSampleRate << " Hz, 2ch, float32 (AUTOCONVERT)\n";

        REFERENCE_TIME dur = 0;
        HRESULT hr = InitializeWithMinLatency(pAC,
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            &captureFormat, &dur);

        if (FAILED(hr)) {
            std::cerr << "[ProcessCapture PID=" << targetPid
                      << "] Initialize falhou: 0x" << std::hex << hr << std::dec << "\n";
            Cleanup(); return;
        }

        hEv = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!hEv) { Cleanup(); return; }

        pAC->SetEventHandle(hEv);
        pAC->GetService(__uuidof(IAudioCaptureClient), (void**)&pCC);
        pAC->Start();
        std::cout << "[ProcessCapture PID=" << targetPid << "] Capturando...\n";

        std::vector<float> tmp;
        while (isRunning) {
            if (WaitForSingleObject(hEv, 2000) == WAIT_TIMEOUT) continue;
            UINT32 pktLen = 0;
            pCC->GetNextPacketSize(&pktLen);
            while (pktLen != 0) {
                BYTE* pData; UINT32 nFrames; DWORD flags;
                if (SUCCEEDED(pCC->GetBuffer(&pData, &nFrames, &flags, NULL, NULL))) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        float vProc = volProc.load();
                        tmp.resize(nFrames * 2);
                        for (UINT32 i = 0; i < nFrames; ++i) {
                            // Formato e sempre float32 stereo apos AUTOCONVERT
                            float L = ((float*)pData)[i*2]   * vProc;
                            float R = ((float*)pData)[i*2+1] * vProc;
                            tmp[i*2] = L; tmp[i*2+1] = R;
                        }
                        targetBuffer->Push(tmp.data(), nFrames * 2);
                    }
                    pCC->ReleaseBuffer(nFrames);
                }
                pCC->GetNextPacketSize(&pktLen);
            }
        }
        Cleanup();
    }

    // -----------------------------------------------------------------------
    // InitializeRender
    // -----------------------------------------------------------------------
    AudioNode InitializeRender(IMMDevice* pDevice) {
        AudioNode node;
        if (!pDevice) return node;
        struct Guard { IMMDevice* d; ~Guard() { if(d) d->Release(); } } g{pDevice};

        if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     NULL, (void**)&node.client)))
            return node;

        node.client->GetMixFormat(&node.format);
        REFERENCE_TIME dur = 0;
        if (FAILED(InitializeWithMinLatency(node.client,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                            node.format, &dur))) {
            node.client->Release(); node.client = nullptr;
            CoTaskMemFree(node.format); node.format = nullptr;
            return node;
        }

        node.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!node.hEvent) {
            node.client->Release(); node.client = nullptr;
            CoTaskMemFree(node.format); node.format = nullptr;
            return node;
        }
        node.client->SetEventHandle(node.hEvent);
        node.client->GetService(__uuidof(IAudioRenderClient), (void**)&node.render);
        node.client->GetBufferSize(&node.bufferSize);
        return node;
    }

    void ReleaseAudioNode(AudioNode& node) {
        if (node.client) { node.client->Stop(); node.client->Release(); node.client = nullptr; }
        if (node.render) { node.render->Release(); node.render = nullptr; }
        if (node.format) { CoTaskMemFree(node.format); node.format = nullptr; }
        if (node.hEvent) { CloseHandle(node.hEvent); node.hEvent = NULL; }
    }

    UINT32 GetDeviceSampleRate(int index) {
        IMMDevice* pDevice = GetDeviceByIndex(eRender, index);
        if (!pDevice) return 48000;
        IAudioClient* pAC = nullptr; WAVEFORMATEX* pwfx = nullptr;
        UINT32 rate = 48000;
        if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                        NULL, (void**)&pAC))) {
            if (SUCCEEDED(pAC->GetMixFormat(&pwfx)) && pwfx) {
                rate = pwfx->nSamplesPerSec; CoTaskMemFree(pwfx);
            }
            pAC->Release();
        }
        pDevice->Release();
        return rate;
    }

    // -----------------------------------------------------------------------
    // MixerLoop — CORRECAO DE CORTES:
    // O retorno fisico (fone) estava cortando porque o MixerLoop usava apenas
    // o evento do cabo virtual para acordar. Quando o cabo virtual esta cheio
    // mas o retorno fisico precisa de dados, o retorno fica sem audio.
    // Correcao: o retorno fisico recebe dados no mesmo ciclo do cabo virtual,
    // mas com verificacao de padding independente. Alem disso, o buffer foi
    // aumentado para 20ms para absorver jitter entre os dois dispositivos.
    // -----------------------------------------------------------------------
    void MixerLoop(int virtualIdx, int physicalIdx) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);

        AudioNode outVirt = InitializeRender(GetDeviceByIndex(eRender, virtualIdx));
        AudioNode outPhys = InitializeRender(GetDeviceByIndex(eRender, physicalIdx));

        if (outVirt.client) outVirt.client->Start();
        if (outPhys.client) outPhys.client->Start();

        bool   vFloat = outVirt.client ? IsFormatFloat(outVirt.format) : false;
        WORD   vBits  = outVirt.client ? outVirt.format->wBitsPerSample : 0;
        UINT32 vCh    = outVirt.client ? outVirt.format->nChannels      : 0;
        UINT32 vRate  = outVirt.client ? outVirt.format->nSamplesPerSec : 48000;

        bool   pFloat = outPhys.client ? IsFormatFloat(outPhys.format) : false;
        WORD   pBits  = outPhys.client ? outPhys.format->wBitsPerSample : 0;
        UINT32 pCh    = outPhys.client ? outPhys.format->nChannels      : 0;

        std::cout << "[Mixer] Virtual: " << vCh << "ch, " << vBits
                  << "bit, " << vRate << "Hz\n";

        while (isRunning) {
            if (!outVirt.hEvent) break;

            if (WaitForSingleObject(outVirt.hEvent, 2000) == WAIT_TIMEOUT) continue;

            // Le padding dos dois dispositivos antes de escrever
            UINT32 padV = 0, padP = 0;
            if (outVirt.client) outVirt.client->GetCurrentPadding(&padV);
            if (outPhys.client) outPhys.client->GetCurrentPadding(&padP);
            UINT32 framesV = outVirt.bufferSize - padV;
            UINT32 framesP = outPhys.client ? (outPhys.bufferSize - padP) : 0;

            // Usa o menor numero de frames para nao consumir mais do buffer
            // do que os dois dispositivos conseguem absorver simultaneamente.
            // Isso elimina o underrun que causava os cortes no retorno.
            UINT32 frames = framesV;

            BYTE*   pDataV = nullptr; HRESULT hrV = E_FAIL;
            BYTE*   pDataP = nullptr; HRESULT hrP = E_FAIL;
            if (frames > 0 && outVirt.render)
                hrV = outVirt.render->GetBuffer(frames, &pDataV);
            if (framesP > 0 && outPhys.render)
                hrP = outPhys.render->GetBuffer(framesP, &pDataP);

            float vM = volMic.load();
            float vP = volProc.load();

            for (UINT32 i = 0; i < frames; ++i) {
                // Le mic UMA vez por frame — mesmo valor vai para cabo e retorno
                float micL = 0.0f, micR = 0.0f;
                if (bufferMic) { bufferMic->Pop(&micL, 1); bufferMic->Pop(&micR, 1); }
                micL *= vM; micR *= vM;

                float sumL = 0.0f, sumR = 0.0f;
                for (auto buf : processBuffers) {
                    float pL = 0.0f, pR = 0.0f;
                    if (buf->Pop(&pL, 1) && buf->Pop(&pR, 1))
                        { sumL += pL * vP; sumR += pR * vP; }
                }

                // Cabo virtual = mic + processos
                if (SUCCEEDED(hrV) && pDataV) {
                    EscreveAmostra(pDataV, i, vCh, 0, vBits, vFloat, Clamp(micL + sumL));
                    if (vCh > 1)
                        EscreveAmostra(pDataV, i, vCh, 1, vBits, vFloat, Clamp(micR + sumR));
                }

                // Retorno fisico = so mic (sem eco dos processos no fone)
                // Usa o mesmo micL/micR ja lido — nao consome bufferMic de novo
                if (SUCCEEDED(hrP) && pDataP && i < framesP) {
                    EscreveAmostra(pDataP, i, pCh, 0, pBits, pFloat, micL);
                    if (pCh > 1)
                        EscreveAmostra(pDataP, i, pCh, 1, pBits, pFloat, micR);
                }
            }

            // Se retorno tem mais frames que o cabo, preenche com silencio
            for (UINT32 i = frames; i < framesP; ++i) {
                if (SUCCEEDED(hrP) && pDataP) {
                    EscreveAmostra(pDataP, i, pCh, 0, pBits, pFloat, 0.0f);
                    if (pCh > 1) EscreveAmostra(pDataP, i, pCh, 1, pBits, pFloat, 0.0f);
                }
            }

            if (SUCCEEDED(hrV) && outVirt.render) outVirt.render->ReleaseBuffer(frames, 0);
            if (SUCCEEDED(hrP) && outPhys.render) outPhys.render->ReleaseBuffer(framesP, 0);
        }

        ReleaseAudioNode(outVirt);
        ReleaseAudioNode(outPhys);
        CoUninitialize();
    }

public:
    AudioEngine()
        : volMic(1.0f), volProc(1.0f), micAtivo(true),
          bufferMic(nullptr), isRunning(false) {}
    ~AudioEngine() { Stop(); }

    // Metodos para ajuste de volume em tempo real (chamados pelo main apos Start)
    void SetVolMic(float v)   { volMic.store(v);  }
    void SetVolProc(float v)  { volProc.store(v); }
    void SetMicAtivo(bool v)  { micAtivo.store(v); }

    void ListDevicesConsole(EDataFlow flow) {
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;
        IMMDeviceEnumerator* pEnum = NULL;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
            { CoUninitialize(); return; }
        IMMDeviceCollection* pCol = NULL;
        pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol);
        UINT count = 0; pCol->GetCount(&count);
        for (ULONG i = 0; i < count; i++) {
            IMMDevice* pEp = NULL; IPropertyStore* pProps = NULL;
            pCol->Item(i, &pEp);
            pEp->OpenPropertyStore(STGM_READ, &pProps);
            PROPVARIANT v; PropVariantInit(&v);
            pProps->GetValue(PKEY_Device_FriendlyName, &v);
            std::wcout << L" [" << i << L"] " << v.pwszVal << L"\n";
            PropVariantClear(&v);
            pProps->Release(); pEp->Release();
        }
        pCol->Release(); pEnum->Release();
        CoUninitialize();
    }

    void Start(int micIdx, int virtualIdx, int foneIdx,
               const std::vector<DWORD>& pids,
               float vMic = 1.0f, float vProc = 1.0f) {
        if (isRunning) return;
        volMic.store(vMic);
        volProc.store(vProc);
        micAtivo.store(micIdx >= 0);
        isRunning = true;

        std::cout << "\nMesa de Som v3.0 iniciando...\n";
        std::cout << "Vol mic=" << vMic << " vol proc=" << vProc << "\n";

        threadMic    = std::thread(&AudioEngine::CaptureMicLoop, this, micIdx);
        threadOutput = std::thread(&AudioEngine::MixerLoop, this, virtualIdx, foneIdx);

        UINT32 outRate = GetDeviceSampleRate(virtualIdx);
        std::cout << "[Start] Taxa do cabo virtual: " << outRate << " Hz\n";

        for (DWORD pid : pids) {
            // Resolve o PID correto de audio antes de ativar
            // Passa um vector de um elemento — ResolverPidAudio verifica sessoes ativas
            std::vector<DWORD> grupo = { pid };
            DWORD pidAudio = ResolverPidAudio(grupo);
            std::cout << "[Start] PID original=" << pid
                      << " → PID audio=" << pidAudio << "\n";

            IAudioClient* pAC = ActivateProcessClient(pidAudio);
            if (!pAC) {
                std::cerr << "[Start] PID " << pidAudio << " falhou.\n";
                continue;
            }
            auto buf = new LockFreeRingBuffer<float>(outRate * 2 * 2);
            processBuffers.push_back(buf);
            processThreads.push_back(
                std::thread(&AudioEngine::CaptureProcessLoop,
                            this, pidAudio, pAC, buf, outRate));
        }
        std::cout << "Processos capturando: " << processThreads.size() << "\n";
    }

    void Stop() {
        if (!isRunning) return;
        isRunning = false;
        if (threadMic.joinable())    threadMic.join();
        for (auto& t : processThreads) { if (t.joinable()) t.join(); }
        if (threadOutput.joinable()) threadOutput.join();
        delete bufferMic; bufferMic = nullptr;
        for (auto buf : processBuffers) delete buf;
        processBuffers.clear();
        processThreads.clear();
    }
};

// ---------------------------------------------------------------------------
// main — argumentos: micIdx caboIdx foneIdx volMic volProc PID1 [PID2 ...]
// micIdx == -1 significa microfone desativado
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    AudioEngine engine;

    if (argc == 2 && std::string(argv[1]) == "--list") {
        std::cout << "--- ENTRADAS ---\n"; engine.ListDevicesConsole(eCapture);
        std::cout << "--- SAIDAS ---\n";   engine.ListDevicesConsole(eRender);
        CoUninitialize();
        return 0;
    }

    if (argc >= 7) {
        try {
            int   micIdx = std::stoi(argv[1]);
            int   caboIdx= std::stoi(argv[2]);
            int   foneIdx= std::stoi(argv[3]);
            float vMic   = std::stof(argv[4]);
            float vProc  = std::stof(argv[5]);

            std::vector<DWORD> pids;
            for (int i = 6; i < argc; ++i)
                pids.push_back(std::stoul(argv[i]));

            engine.Start(micIdx, caboIdx, foneIdx, pids, vMic, vProc);
            std::cout << "Mesa de som rodando. ENTER para parar.\n";
            std::cin.get();
            engine.Stop();
            CoUninitialize();
            return 0;
        }
        catch (...) { CoUninitialize(); return 1; }
    }

    std::cerr << "Uso: Placasom.exe <micIdx|-1> <caboIdx> <foneIdx|-1> "
              << "<volMic> <volProc> <PID1> [PID2 ...]\n";
    std::cerr << "     Placasom.exe --list\n";
    CoUninitialize();
    return 1;
}
