// VARM.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdint.h>
#include "defs.h"
#include <d3dx9.h>
#include <d3d9.h>
#include <ddraw.h>
#include "resource.h"

extern void core_decode(uchar* opcodes);
extern void core_init(uchar* buffer, int len);
extern uchar core_exec(uchar* vbuffer);
//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------
static HWND hWnd;
LPDIRECT3D9					g_pD3D = NULL; // Used to create the D3DDevice
LPDIRECT3DDEVICE9			g_pd3dDevice = NULL; // Our rendering device
LPDIRECT3DVERTEXBUFFER9		g_pVB = NULL; // Buffer to hold vertices
LPDIRECT3DTEXTURE9			g_pTexture = NULL; // Our texture
LPDIRECT3DSURFACE9			g_pSurface = NULL;

#define VM_WINDOW_WIDTH		512
#define VM_WINDOW_HEIGHT	480
#define VM_LCD_WIDTH		512
#define VM_LCD_HEIGHT		480
#define VM_LCD_POS_Y 0
#define VM_LCD_POS_X 0

static uchar _lcdbuffer[VM_LCD_WIDTH * VM_LCD_HEIGHT * 4];

//-----------------------------------------------------------------------------
// Name: InitD3D()
// Desc: Initializes Direct3D
//-----------------------------------------------------------------------------
HRESULT InitD3D(HWND hWnd)
{
	IDirect3DSurface9* m_pBackgroundSurface;
	HRESULT ddrval;
	// Create the D3D object.
	if (NULL == (g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)))
		return E_FAIL;
	RECT _rc_source = { 0, 0, VM_WINDOW_WIDTH, VM_WINDOW_HEIGHT };
	// Set up the structure used to create the D3DDevice. Since we are now
	// using more complex geometry, we will create a device with a zbuffer.
	D3DPRESENT_PARAMETERS d3dpp;
	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = TRUE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.BackBufferFormat = D3DFMT_R5G6B5;
	d3dpp.EnableAutoDepthStencil = FALSE;
	d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
	// Create the D3DDevice
	if (FAILED(g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING,
		&d3dpp, &g_pd3dDevice)))
	{
		return E_FAIL;
	}
	// Turn off culling
	g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	// Turn off D3D lighting
	g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
	// Turn on the zbuffer
	g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, TRUE);

	g_pd3dDevice->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);


	g_pd3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_pBackgroundSurface);
	
	memset(_lcdbuffer, 0, sizeof(_lcdbuffer));
	if (SUCCEEDED(ddrval = D3DXLoadSurfaceFromMemory(
		m_pBackgroundSurface,
		NULL,
		NULL,
		_lcdbuffer,
		D3DFMT_A8R8G8B8,
		4 * VM_LCD_WIDTH,
		NULL,
		&_rc_source,
		D3DX_FILTER_NONE,
		0)
		)) {

	}
	return S_OK;
}
// The one and only application object

CWinApp theApp;

using namespace std;


VOID SetupMatrices(uint32 address)
{
	IDirect3DSurface9* m_pBackgroundSurface;
	IDirect3DSurface9* m_pLCDSurface;
	HRESULT ddrval;
	uchar bpp_mode = (uchar)0xb;
	//uchar frm565 = (uchar)(lcd_registers[PLCDCON5] >> 11) & 0x01;
	D3DFORMAT p_format = D3DFMT_R8G8B8;
	uint32 p_size;
	uint16 hozval = (uint16)VM_LCD_WIDTH ;
	RECT _rc_dst = { 0, 0, VM_LCD_WIDTH, VM_LCD_HEIGHT };
	POINT _p_dst = { VM_LCD_POS_X, VM_LCD_POS_Y };
	g_pd3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_pBackgroundSurface);
#if	LCDDEBUG
	printf("hozval : %x\n", hozval);
#endif
	switch (bpp_mode) {
	case 0x8:		//1 bpp for TFT
		p_format = D3DFMT_UNKNOWN;
#if	LCDDEBUG
		printf("format : D3DFMT_UNKNOWN\n");
#endif
		break;
	case 0x9:		//2 bpp for TFT
		p_format = D3DFMT_UNKNOWN;
#if	LCDDEBUG
		printf("format : D3DFMT_UNKNOWN\n");
#endif
		break;
	case 0xa:		//4 bpp for TFT
		p_format = D3DFMT_UNKNOWN;
#if	LCDDEBUG
		printf("format : D3DFMT_UNKNOWN\n");
#endif
		break;
	case 0xb:		//8 bpp for TFT
		p_format = D3DFMT_R3G3B2;
#if	LCDDEBUG
		printf("format : D3DFMT_R3G3B2\n");
#endif
		p_size = 1;
		break;
	case 0xc:		//16 bpp for TFT
		p_size = 2;
		break;
	case 0xd:		//24 bpp for TFT
		p_format = D3DFMT_R8G8B8;
#if	LCDDEBUG
		printf("format : D3DFMT_R8G8B8\n");
#endif
		p_size = 3;
		break;
	default:
		p_format = D3DFMT_UNKNOWN;
#if	LCDDEBUG
		printf("format : D3DFMT_UNKNOWN\n");
#endif
		p_size = 3;
		break;
	}
	//memset(_lcdbuffer, 0x1C, sizeof(_lcdbuffer));
	//HRESULT CreateSurface9( LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE FAR* lplpDDSurface, NULL);
	//CreateOffscreenPlainSurface9
	if (SUCCEEDED(ddrval = D3DXLoadSurfaceFromMemory(
		m_pBackgroundSurface,
		NULL,
		NULL,
		_lcdbuffer,
		D3DFMT_A8R8G8B8,
		4 * VM_LCD_WIDTH,
		NULL,
		&_rc_dst,
		D3DX_FILTER_NONE,
		0
		))) {
		//printf("surface loaded\n");
	}

}


//-----------------------------------------------------------------------------
// Name: Cleanup()
// Desc: Releases all previously initialized objects
//-----------------------------------------------------------------------------
void Cleanup()
{
	if (g_pVB != NULL)
		g_pVB->Release();

	if (g_pd3dDevice != NULL)
		g_pd3dDevice->Release();

	if (g_pD3D != NULL)
		g_pD3D->Release();
}
//-----------------------------------------------------------------------------
// Name: Render()
// Desc: Draws the scene
//-----------------------------------------------------------------------------
void Render()
{
	static uint32 address;
	IDirect3DSurface9* m_pBackgroundSurface;
	HRESULT ddrval;
	RECT _rc_source = { 0, 0, 467, 427 };
	uchar envid = (uchar)1;
	uchar pwren = (uchar)1;
	if (pwren && envid) {
		// Clear the backbuffer and the zbuffer
		g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
			D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
		// Begin the scene
		if (SUCCEEDED(g_pd3dDevice->BeginScene()))
		{
			SetupMatrices(address);
			g_pd3dDevice->EndScene();
		}
	}
	else {
		// Clear the backbuffer and the zbuffer
		g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
			D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
		// Begin the scene
	}

	// Present the backbuffer contents to the display
	g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
	//g_pd3dDevice->clea
}

//-----------------------------------------------------------------------------
// Name: MsgProc()
// Desc: The window's message handler
//-----------------------------------------------------------------------------
LRESULT WINAPI MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HICON hIcon, hIconSm;
	switch (msg)
	{
	case WM_CREATE:
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		default: break;
		}
		break;
	case WM_DESTROY:
		//Cleanup();
		//_active_core->command(VM_QUIT, 0, NULL);
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

char codespace[65536 * 16];
int _tmain(int argc, CHAR* argv[], CHAR* envp[])
{
	HMENU hMenu, hSubMenu;
	WIN32_FIND_DATA FindFileData;
   	HANDLE hFind;
	int nRetCode = 0;
	int16 result = 0;
	uint32 code_length;
	char filename[256];
	WNDCLASSEX wc =
	{
		sizeof(WNDCLASSEX), CS_CLASSDC, MsgProc, 0L, 0L,
		GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
		LPCTSTR(L"VNES"), NULL
	};
	wc.hIcon = static_cast<HICON>(LoadImage(wc.hInstance,
		MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON,
		32,
		32,
		LR_DEFAULTSIZE));
	wc.hIconSm = static_cast<HICON>(LoadImage(wc.hInstance,
		MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON,
		16,
		16,
		LR_DEFAULTSIZE));
	wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
	wc.hIconSm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 16, 16, 0);
	wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	RegisterClassEx(&wc);

	hMenu = CreateMenu();
	//wchar_t * dirname = _T(".\\plugins\\*");
	size_t i;
	//memcpy(varm_config.global, LPCTSTR(L"smdk2440.dll"), 256);

	// initialize MFC and print and error on failure
	if (!AfxWinInit(::GetModuleHandle(NULL), NULL, ::GetCommandLine(), 0))
	{
		// TODO: change error code to suit your needs
		_tprintf(_T("Fatal Error: MFC initialization failed\n"));
		nRetCode = -1;
	}
	else
	{

		hWnd = CreateWindow(LPCTSTR(L"VNES"), LPCTSTR(L"VNES"),
			WS_POPUPWINDOW | WS_CAPTION, 0, 0, VM_WINDOW_WIDTH, VM_WINDOW_HEIGHT,
			NULL, hMenu, wc.hInstance, NULL);
		// TODO: code your application's behavior here.
		// Initialize Direct3D
		if (SUCCEEDED(InitD3D(hWnd)))
		{
			//for touchscreen interface
			//initDInput(wc.hInstance, hWnd);
			// Create the scene geometry
			//if( SUCCEEDED( InitGeometry() ) )
			{
				// Show the window
				ShowWindow(hWnd, SW_SHOWDEFAULT);
				UpdateWindow(hWnd);
			}
		}
		// Enter the message loop
		MSG msg;
		ZeroMemory(&msg, sizeof(msg));
		ShowWindow(hWnd, SW_SHOWDEFAULT);
		UpdateWindow(hWnd);
		char buffer[256];
		int len;
		int index = 0;
		FILE * ff = fopen("D:\\Workspace\\VSProjects\\VNES\\debug\\SMB.nes", "rb");
		while ((len = fread(buffer, 1, 256, ff)) == 256) {
			memcpy(codespace + index, buffer, len);
			index += len;
		}
		memcpy(codespace + index, buffer, len);
		index += len;
		core_init((uchar *)codespace, index);
		while (msg.message != WM_QUIT)
		{
			if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			else {
				if (core_exec((uchar *)_lcdbuffer)) {
					Render();
				}
				//Sleep(10);
				//printf("%x\n", _active_core->address);
			}
		}
		Cleanup();
		UnregisterClass(LPCTSTR(L"VNES"), wc.hInstance);
		
	}

	return nRetCode;
}
