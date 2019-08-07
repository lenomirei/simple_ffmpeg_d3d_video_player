#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <iostream>


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "libavutil/imgutils.h"
}

#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib,"ws2_32.lib")

CRITICAL_SECTION  m_critial;

IDirect3D9 *m_pDirect3D9 = NULL;
IDirect3DDevice9 *m_pDirect3DDevice = NULL;
IDirect3DSurface9 *m_pDirect3DSurfaceRender = NULL;

RECT m_rtViewport;

//Width, Height
const int screen_w = 1280, screen_h = 720;
const int pixel_w = 1280, pixel_h = 720;

//Bit per Pixel
const int bpp = 12;

void Cleanup()
{
    EnterCriticalSection(&m_critial);
    if (m_pDirect3DSurfaceRender)
        m_pDirect3DSurfaceRender->Release();
    if (m_pDirect3DDevice)
        m_pDirect3DDevice->Release();
    if (m_pDirect3D9)
        m_pDirect3D9->Release();
    LeaveCriticalSection(&m_critial);
}

int InitD3D(HWND hwnd, unsigned long lWidth, unsigned long lHeight)
{
    HRESULT lRet;
    InitializeCriticalSection(&m_critial);
    Cleanup();

    m_pDirect3D9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (m_pDirect3D9 == NULL)
        return -1;

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;

    GetClientRect(hwnd, &m_rtViewport);

    lRet = m_pDirect3D9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &d3dpp, &m_pDirect3DDevice);
    if (FAILED(lRet))
        return -1;

    lRet = m_pDirect3DDevice->CreateOffscreenPlainSurface(
        lWidth, lHeight,
        (D3DFORMAT)MAKEFOURCC('Y', 'V', '1', '2'),
        D3DPOOL_DEFAULT,
        &m_pDirect3DSurfaceRender,
        NULL);


    if (FAILED(lRet))
        return -1;

    return 0;
}

LRESULT WINAPI MyWndProc(HWND hwnd, UINT msg, WPARAM wparma, LPARAM lparam)
{
    switch (msg) {
    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wparma, lparam);
}

int OpenCodecContext(int *stream_idx,
    AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type, char* src_filename)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
            av_get_media_type_string(type), src_filename);
        return ret;
    }
    else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders, with or without reference counting */
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

#define INBUF_SIZE 1400

int WINAPI WinMain(__in HINSTANCE hInstance, __in_opt HINSTANCE hPrevInstance, __in LPSTR lpCmdLine, __in int nShowCmd)
{
    WSADATA WSAData;
    WORD sockVersion = MAKEWORD(2, 2);
    if (WSAStartup(sockVersion, &WSAData) != 0)
        return 0;

    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *codec_context = NULL;
    AVFrame *frame,*frame_YUV;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    unsigned char *out_buffer = NULL;
    int ret;
    AVPacket *packet;
    int y_size;
    struct SwsContext *img_convert_ctx = NULL;

    SOCKET serSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);	//����������socket
    if (INVALID_SOCKET == serSocket)
    {
        std::cout << "socket error!" << std::endl;
        return 0;
    }
    //���ô���Э�顢�˿��Լ�Ŀ�ĵ�ַ 
    sockaddr_in serAddr;
    serAddr.sin_family = AF_INET;
    serAddr.sin_port = htons(1234);
    serAddr.sin_addr.S_un.S_addr = INADDR_ANY;

    if (bind(serSocket, (sockaddr*)&serAddr, sizeof(serAddr)) == SOCKET_ERROR)	 //��socket�󶨵�ַ 
    {
        std::cout << "bind error" << std::endl;
        closesocket(serSocket);
        return 0;
    }
    sockaddr_in clientAddr;
    int iAddrlen = sizeof(clientAddr);

    //�����鲥��
    ip_mreq multiCast;
    multiCast.imr_interface.S_un.S_addr = INADDR_ANY;		//����ĳһ�����豸�ӿڵ�IP��ַ��
    multiCast.imr_multiaddr.S_un.S_addr = inet_addr("224.4.5.6");	//�鲥���IP��ַ��
    setsockopt(serSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&multiCast, sizeof(multiCast));
    int receive_buf_size = 65536 * 10;
    if (setsockopt(serSocket, SOL_SOCKET, SO_RCVBUF, (char*)&receive_buf_size, sizeof(receive_buf_size)) < 0)
    {
        std::cout << "cccccccccccc" << std::endl;
    }

    packet = av_packet_alloc();
    if (!packet)
        exit(1);

    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /* find the MPEG-1 video decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }

    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

       /* open it */
    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }


    frame = av_frame_alloc();
    frame_YUV = av_frame_alloc();
    if (!frame || !frame_YUV) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 1280, 720, 1));
    av_image_fill_arrays(frame_YUV->data, frame_YUV->linesize, out_buffer,
        AV_PIX_FMT_YUV420P, 1280, 720, 1);

    img_convert_ctx = sws_getContext(1280, 720, AV_PIX_FMT_YUV420P,
        1280, 720, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));

    wc.cbSize = sizeof(wc);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpfnWndProc = (WNDPROC)MyWndProc;
    wc.lpszClassName = L"D3D";
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassExW(&wc);

    HWND hwnd = NULL;
    hwnd = CreateWindowW(L"D3D", L"Simplest Video Play Direct3D (Surface)", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, NULL, NULL, hInstance, NULL);
    if (hwnd == NULL)
    {
        return -1;
    }

    if (InitD3D(hwnd, pixel_w, pixel_h) == E_FAIL)
    {
        return -1;
    }

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (msg.message != WM_QUIT)
    {
        //PeekMessage, not GetMessage
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            //Sleep(10);
            memset(inbuf, 0, INBUF_SIZE);
            data_size = recvfrom(serSocket, (char *)inbuf, INBUF_SIZE, 0, (sockaddr*)&clientAddr, &iAddrlen);
            if (!data_size)
                continue;
            data = inbuf;
            while (data_size > 0)
            {
                ret = av_parser_parse2(parser, codec_context, &packet->data, &packet->size,
                    data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                data += ret;
                data_size -= ret;

                if (packet->size) {

                    ret = avcodec_send_packet(codec_context, packet);
                    if (ret < 0)
                    {
                        fprintf(stderr, "Error sending a packet for decoding\n");
                    }
                    if (ret >= 0)
                    {
                        ret = avcodec_receive_frame(codec_context, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            continue;
                        else if (ret < 0)
                        {
                            printf("Decode error\n");
                        }
                        sws_scale(img_convert_ctx, (const unsigned char* const*)frame->data, frame->linesize, 0, 720,
                            frame_YUV->data, frame_YUV->linesize);
                        y_size = 1280*720;

                        HRESULT lRet;
                        if (m_pDirect3DSurfaceRender == NULL)
                            return -1;
                        D3DLOCKED_RECT d3d_rect;
                        lRet = m_pDirect3DSurfaceRender->LockRect(&d3d_rect, NULL, D3DLOCK_DONOTWAIT);
                        if (lRet == D3DERR_WASSTILLDRAWING) {
                            //  surface still drawing.
                            //Sleep(100);
                            continue;
                        }

                        if (FAILED(lRet))
                            return -1;
                        byte * pDest = (BYTE *)d3d_rect.pBits;
                        int stride = d3d_rect.Pitch;
                        unsigned long i = 0;
                        for (i = 0; i < pixel_h; i++) { // Y
                            memcpy(pDest + i * stride, frame_YUV->data[0] + i * pixel_w, pixel_w);
                        }
                        for (i = 0; i < pixel_h / 2; i++) { // V
                            memcpy(pDest + stride * pixel_h + i * stride / 2, frame_YUV->data[2] + i * pixel_w / 2, pixel_w / 2);
                        }
                        for (i = 0; i < pixel_h / 2; i++) { // U
                            memcpy(pDest + stride * pixel_h + stride * pixel_h / 4 + i * stride / 2, frame_YUV->data[1] + i * stride / 2, pixel_w / 2);
                        }

                        lRet = m_pDirect3DSurfaceRender->UnlockRect();
                        if (FAILED(lRet))
                            return -1;

                        if (m_pDirect3DDevice == NULL)
                            return -1;

                        m_pDirect3DDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
                        m_pDirect3DDevice->BeginScene();
                        IDirect3DSurface9 * pBackBuffer = NULL;

                        m_pDirect3DDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
                        m_pDirect3DDevice->StretchRect(m_pDirect3DSurfaceRender, NULL, pBackBuffer, &m_rtViewport, D3DTEXF_LINEAR);
                        m_pDirect3DDevice->EndScene();
                        m_pDirect3DDevice->Present(NULL, NULL, NULL, NULL);
                    }
                }
            }
        }
    }

    UnregisterClassW(L"D3D", hInstance);

    closesocket(serSocket);
    WSACleanup();
    return 0;
}