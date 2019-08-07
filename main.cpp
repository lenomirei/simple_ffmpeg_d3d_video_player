#include <windows.h>
#include <stdio.h>
#include <d3d9.h>


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "libavutil/imgutils.h"
}

CRITICAL_SECTION  m_critial;

IDirect3D9 *m_pDirect3D9 = NULL;
IDirect3DDevice9 *m_pDirect3DDevice = NULL;
IDirect3DSurface9 *m_pDirect3DSurfaceRender = NULL;

RECT m_rtViewport;

//Width, Height
const int screen_w = 1280, screen_h = 720;
const int pixel_w = 1280, pixel_h = 720;
FILE *fp = NULL;

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

int WINAPI WinMain(__in HINSTANCE hInstance, __in_opt HINSTANCE hPrevInstance, __in LPSTR lpCmdLine, __in int nShowCmd) 
{
	AVFormatContext	*format_context = NULL;
	static int video_stream_idx = -1;
	AVCodecContext	*codec_context = NULL;
	AVCodec			*codec = NULL;
	AVFrame	*frame, *frame_YUV = NULL;
	unsigned char *out_buffer = NULL;
	AVPacket packet;
	AVStream *video_stream = NULL;
	enum AVPixelFormat pix_fmt;
	int y_size;
	int width, height;
	int ret;
	struct SwsContext *img_convert_ctx = NULL;

	char filepath[] = "【MV】Lucky☆Orb feat. Hatsune Miku by emon(Tes.)  - ラッキー☆オーブ feat. 初音ミク by emon(Tes.) 【MIKU EXPO 5th】.webm";
	//char filepath[] = "udp://@224.4.5.6:1234";

	if (avformat_open_input(&format_context, filepath, NULL, NULL) < 0) 
	{
		fprintf(stderr, "Could not open source file %s\n", filepath);
		exit(1);
	}

	if (avformat_find_stream_info(format_context, NULL) < 0) 
	{
		fprintf(stderr, "Could not find stream information\n");
		exit(1);
	}

	if (OpenCodecContext(&video_stream_idx, &codec_context, format_context, AVMEDIA_TYPE_VIDEO, filepath) >= 0) 
	{
		video_stream = format_context->streams[video_stream_idx];

		/* allocate image where the decoded image will be put */
		width = codec_context->width;
		height = codec_context->height;
		pix_fmt = codec_context->pix_fmt;
	}
	if (!video_stream) 
	{
		fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
		return -1;
	}

	frame = av_frame_alloc();
	frame_YUV = av_frame_alloc();
	
	if (!frame || !frame_YUV)
	{
		fprintf(stderr, "Could not allocate frame\n");
		return AVERROR(ENOMEM);
	}

    out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codec_context->width, codec_context->height, 1));
    av_image_fill_arrays(frame_YUV->data, frame_YUV->linesize, out_buffer,
        AV_PIX_FMT_YUV420P, codec_context->width, codec_context->height, 1);

	av_init_packet(&packet);
	packet.data = NULL;
	packet.size = 0;

	img_convert_ctx = sws_getContext(codec_context->width, codec_context->height, codec_context->pix_fmt,
		codec_context->width, codec_context->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

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
			Sleep(10);
			// Render();
			if (av_read_frame(format_context, &packet) >= 0) 
			{
				if (packet.stream_index == video_stream_idx) 
				{
					ret = avcodec_send_packet(codec_context, &packet);
					if (ret < 0)
					{
						fprintf(stderr, "Error sending a packet for decoding\n");
					}
					while (ret >= 0) 
					{
						ret = avcodec_receive_frame(codec_context, frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						else if (ret < 0) 
						{
							printf("Decode error\n");
						}
						sws_scale(img_convert_ctx, (const unsigned char* const*)frame->data, frame->linesize, 0, codec_context->height,
							frame_YUV->data, frame_YUV->linesize);
						y_size = codec_context->width*codec_context->height;

						HRESULT lRet;
						if (m_pDirect3DSurfaceRender == NULL)
							return -1;
						D3DLOCKED_RECT d3d_rect;
						lRet = m_pDirect3DSurfaceRender->LockRect(&d3d_rect, NULL, D3DLOCK_DONOTWAIT);
                        if (lRet == D3DERR_WASSTILLDRAWING) {
                            //  surface still drawing.
                            // 这样处理会丢掉一些视频帧，按理说应该有个缓冲区等待渲染才对
                            Sleep(100);
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

	return 0;
}