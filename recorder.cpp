// recorder.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <windows.h>
#include <initguid.h>
//#include <mmeapi.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <atlcomcli.h>
#include <Functiondiscoverykeys_devpkey.h>

#include "lame-3.100/include/lame.h"


#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000
#pragma comment(lib,"Winmm.lib")
#pragma comment(lib,"lame-3.100/output/libmp3lame-static.lib")

class Loger {
    std::stringstream os;
    std::stringstream err;
public:
    Loger() {

    }
    ~Loger() {
        if (!os.str().empty())
            std::cout << os.str() << std::endl;
        if (!err.str().empty())
            std::cerr << err.str() << std::endl;
    }
    std::stringstream& Info() {
        return os;
    }
    std::stringstream& Err() {
        return err;
    }
};

#define ERR() Loger().Err()
#define INFO() Loger().Info()


struct WAVE_HEADER {
    char RIFF[4] = {'R','I','F','F'};
    unsigned int size = 0;
    char WAVE[4] = {'W','A','V','E'};
    char FMT[4] = { 'f','m','t',' ' };
    unsigned int  fmt_size = sizeof(PCMWAVEFORMAT);
    WAVEFORMAT wavfmt;
    WORD        wBitsPerSample;
    char DATA[4] = { 'D','A','T','A' };
    unsigned int data_size = 0;
};

BOOL AdjustFormatTo16Bits(WAVEFORMATEX* pwfx)
{
    BOOL bRet(FALSE);

    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->wBitsPerSample = 16;
        pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
        pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;

        bRet = TRUE;
    }
    else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
        if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat))
        {
            pEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            pEx->Samples.wValidBitsPerSample = 16;
            pwfx->wBitsPerSample = 16;
            pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
            pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;

            bRet = TRUE;
        }
    }

    return bRet;
}

HANDLE hStopEvent = nullptr;
HANDLE hExitEvent = nullptr;
void write_mp3(lame_t& lame, LPBYTE data, UINT32 size, int block_algin, FILE* fp)
{
    std::string mp3_buf;
    mp3_buf.resize(size);
    int enc_size = lame_encode_buffer_interleaved(lame, (short*)data, size / block_algin,
        (unsigned char*)mp3_buf.data(), size);
    if (enc_size > 0)
    {
        fwrite(mp3_buf.data(), enc_size, 1, fp);
    }
}

void capture(const char* file) {
    INFO() << "begin. capture to file: " << file;
    CoInitialize(NULL);
    CComPtr<IMMDeviceEnumerator> mm_dev_enum;
    mm_dev_enum.CoCreateInstance(__uuidof(MMDeviceEnumerator));
    if (!mm_dev_enum)
    {
        ERR() << "can't creat enumerator.";
        exit(1);
    }
    CComPtr<IMMDeviceCollection> mmdc;
    mm_dev_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &mmdc);
    if (!mmdc)
    {
        ERR() << "Can't get colletion obj.";
        exit(1);
    }
    CComPtr<IMMDevice> DefaultDev;
    HRESULT hr = S_OK;
    mm_dev_enum->GetDefaultAudioEndpoint(eRender, ERole::eConsole, &DefaultDev);
    if (!DefaultDev)
    {
        ERR() << "Can't get default device.";
        exit(1);
    }
    CComPtr<IAudioClient> Client;
    DefaultDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&Client);
    if (!Client)
    {
        ERR() << "Can't get audio client.";
        exit(1);
    }
    WAVEFORMATEX *wave_fmt = nullptr;
    Client->GetMixFormat(&wave_fmt);
    AdjustFormatTo16Bits(wave_fmt);
    wave_fmt->wFormatTag = WAVE_FORMAT_PCM;
//     wave_fmt->nChannels = 2;
//     wave_fmt->nSamplesPerSec = 48000;
//     wave_fmt->nAvgBytesPerSec = 192000;
//     wave_fmt->wBitsPerSample = 16;
//     wave_fmt->nBlockAlign = 4;
     wave_fmt->cbSize = 0;


    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;

    hr = Client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsRequestedDuration, 0, wave_fmt, nullptr);
    if (FAILED(hr))
    {
        ERR() << "Initialize client failed. " << hr;
        exit(1);
    }

//     FILE* wav_file = nullptr;
//     fopen_s(&wav_file, "e:\\test.wav", "wb");
//     if (wav_file == nullptr)
//     {
//         std::cerr << "Can't open wave file. " << std::endl;
//         exit(1);
//     }

    CComPtr<IAudioCaptureClient> Capture;
    Client->GetService(__uuidof(IAudioCaptureClient), (void**)&Capture);

    UINT32 bufferFrameCount = 0;
    hr = Client->GetBufferSize(&bufferFrameCount);
    REFERENCE_TIME hnsActualDuration = (REFERENCE_TIME)
        ((double)hnsRequestedDuration * bufferFrameCount / wave_fmt->nSamplesPerSec);
    REFERENCE_TIME      hnsDefaultDevicePeriod(0);
    LARGE_INTEGER liFirstFire;
    
    HANDLE  hTimerWakeUp = CreateWaitableTimer(NULL, FALSE, NULL);
    liFirstFire.QuadPart = -hnsDefaultDevicePeriod / 2; // negative means relative time
    LONG lTimeBetweenFires = (LONG)(hnsActualDuration / REFTIMES_PER_MILLISEC / 2);
    SetWaitableTimer(hTimerWakeUp, &liFirstFire, lTimeBetweenFires, NULL, NULL, FALSE);

    hr = Client->Start();
    HANDLE wait[2] = { hTimerWakeUp ,hStopEvent };
    UINT32                  packetLength = 0;

    lame_t lame = lame_init();
    lame_set_in_samplerate(lame, 44100);
    //lame_set_VBR(lame, vbr_default);
    lame_set_VBR(lame, vbr_off);
    lame_set_quality(lame, 5);
    lame_set_brate(lame, 128);
    lame_set_mode(lame, JOINT_STEREO);
    lame_set_num_channels(lame, 2);
    lame_set_bWriteVbrTag(lame, 1);
    lame_init_params(lame);

    FILE* fp = nullptr;
    fopen_s(&fp, file/*"e:\\123.mp3"*/, "wb");
    //FILE* fp_wav = nullptr;
    //fopen_s(&fp_wav, "e:\\123.wav", "wb");
    WAVE_HEADER header;
    header.size = sizeof(header) - sizeof(header.RIFF) - sizeof(header.size);
    header.wBitsPerSample = wave_fmt->wBitsPerSample;
    memcpy(&header.wavfmt, wave_fmt, sizeof(header.wavfmt));
    //fwrite(&header, sizeof(header), 1, fp_wav);
    header.data_size = 0;
    
    while (true)
    {
        DWORD dw = WaitForMultipleObjects(sizeof(wait) / sizeof(wait[0]), wait, FALSE, INFINITE);
        if (dw == WAIT_OBJECT_0 + 1)
            break;
        hr = Capture->GetNextPacketSize(&packetLength);
        INFO() << "==";
        std::string prog = "";
        while (packetLength != 0)
        {
            // Get the available data in the shared buffer.
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            hr = Capture->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            //fwrite(pData, numFramesAvailable* wave_fmt->nBlockAlign, 1, fp_wav);
            header.data_size += numFramesAvailable* wave_fmt->nBlockAlign;
            write_mp3(lame, pData, numFramesAvailable* wave_fmt->nBlockAlign, wave_fmt->nBlockAlign, fp);
                //if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                //{
                //  pData = NULL;  // Tell CopyData to write silence.
                //}

                // Copy the available capture data to the audio sink.
                //hr = pMySink->CopyData((char*)pData, numFramesAvailable * pwfx->nBlockAlign, &bDone);
           

            hr = Capture->ReleaseBuffer(numFramesAvailable);
            

            hr = Capture->GetNextPacketSize(&packetLength);
            prog += ".";
            
        }
        std::cout << prog;
    }
    Client->Stop();
    header.size = 24+header.data_size;
    //fseek(fp_wav, 0, SEEK_SET);
    //fwrite(&header, sizeof(header), 1, fp_wav);
    //fseek(fp_wav, 0, SEEK_END);
    //fclose(fp_wav);
    SetEvent(hExitEvent);
}

int main()
{
    INFO() << "starting...";

    OPENFILENAMEA opf = { 0 };
    char file[1024] = { 0 };
    opf.lStructSize = sizeof(opf);
    opf.Flags = OFN_SHOWHELP | OFN_OVERWRITEPROMPT;
    opf.lpstrFile = file;
    opf.lpstrFilter = "*.mp3\0*.mp3\0\0";
    opf.nMaxFile = 1024;
    opf.lpstrDefExt = "mp3";
    if (!GetSaveFileNameA(&opf))
    {
        return 0;
    }
    hStopEvent = CreateEvent(NULL, FALSE, FALSE, L"StopEvent.AudioRecord");
    hExitEvent = CreateEvent(NULL, FALSE, FALSE, L"ExitEvent.AduioRecord");

    std::thread t([file] {capture(file); });
    
    std::cout << "press entry to exit."<<std::endl;
    char c[1] = { 0 };
    std::cin.read(c, 1);
    
    SetEvent(hStopEvent);

    t.join();
    
    return 0;
    

    auto num = waveInGetNumDevs();
    if (num <= 0)
    {
        std::cerr << "No input devices.";
        return 1;
    }

    std::cout << "There is(are) " << num << " input device(s)"<<std::endl;
    std::vector<WAVEINCAPS> wavein_caps;
    for (UINT id =0; id<num; id++)
    {
        WAVEINCAPS caps = { 0 };
        
        auto r = waveInGetDevCaps(id, &caps, sizeof(WAVEINCAPS));
        if (MMSYSERR_NOERROR != r)
        {
            std::cerr << "Get Input Device Caps Error. id=" << id << " error code=" << r;
            return 1;
        }
        wavein_caps.push_back(caps);
        std::wcout << id << L" " << caps.szPname<<std::endl;
    }
    lame_t lame = lame_init();
    lame_set_in_samplerate(lame, 44100);
    lame_set_VBR(lame, vbr_default);
    lame_init_params(lame);
    CoInitialize(NULL);
    CComPtr<IMMDeviceEnumerator> mm_dev_enum;
    mm_dev_enum.CoCreateInstance(__uuidof(MMDeviceEnumerator));
    if (mm_dev_enum)
    {
        CComPtr<IMMDeviceCollection> mmdc;
        mm_dev_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &mmdc);
        if (mmdc)
        {
            UINT count = 0;
            mmdc->GetCount(&count);
            for (UINT i=0; i<count;i++)
            {
                CComPtr<IMMDevice> dev;
                mmdc->Item(i, &dev);
                if (dev)
                {
                    CComPtr<IPropertyStore> pProps ;
                    PROPVARIANT varName;
                    // Initialize container for property value.
                    PropVariantInit(&varName);
                    dev->OpenPropertyStore(STGM_READ, &pProps);
                    pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                    std::wcout << "device name: " << varName.bstrVal << std::endl;
                    PropVariantClear(&varName);
                }
            }
        }
        CComPtr<IMMDevice> DefaultDev;
        HRESULT hr = S_OK;
        mm_dev_enum->GetDefaultAudioEndpoint(eRender, ERole::eConsole, &DefaultDev);
        CComPtr<IPropertyStore> pProps;
        DefaultDev->OpenPropertyStore(STGM_READ, &pProps);
        PROPVARIANT varName;
        PropVariantInit(&varName);
        pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        std::wcout << L"default device name:" << varName.bstrVal << std::endl;
        PropVariantClear(&varName);

        CComPtr<IAudioClient> audioClient;
        DefaultDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        UINT buf_size = 0;
        WAVEFORMATEX *fmt = nullptr;
        audioClient->GetMixFormat(&fmt);
        REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
        audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            hnsRequestedDuration,
            0,
            fmt,
            0
        );
        audioClient->GetBufferSize(&buf_size);
        CComPtr<IAudioCaptureClient> capture;
        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        REFERENCE_TIME hnsActualDuration = (double)REFTIMES_PER_SEC *
            buf_size / fmt->nSamplesPerSec;
        audioClient->Start();
        DWORD start = GetTickCount();
        FILE* mp3 = nullptr;
        fopen_s(&mp3,"e:\\test.mp3", "wb");
        while (true)
        {
            Sleep(hnsActualDuration / REFTIMES_PER_MILLISEC / 2);

            UINT pack_len = 0;
            capture->GetNextPacketSize(&pack_len);
            std::string buf;
            UINT buf_size = 0;
            while (pack_len != 0)
            {
                BYTE* data = nullptr;
                UINT frames = 0;
                DWORD flags = 0;
                capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                {
                    data = nullptr;
                }
                if (data)
                {
                    buf_size += frames;
                    buf.resize(frames + buf.size());
                    buf.append((char*)data, frames);
                }
                capture->ReleaseBuffer(frames);
                capture->GetNextPacketSize(&pack_len);
            }
            
            std::string mp3_buf;
            mp3_buf.resize(buf_size);
            int enc_size = lame_encode_buffer_interleaved(lame, (short*)buf.data(), buf_size / sizeof(short),
                (unsigned char*)mp3_buf.data(), buf_size);
            if (enc_size > 0)
            {
                fwrite(mp3_buf.data(), enc_size, 1, mp3);
            }

            if ((GetTickCount() - start) / 1000 > 10)
            {
                break;
            }

        }
        lame_mp3_tags_fid(lame, mp3);
        fclose(mp3);
        audioClient->Stop();
        
    }
    


    return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
