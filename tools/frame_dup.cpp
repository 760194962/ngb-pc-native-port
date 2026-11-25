// frame_dup: which game frame the screen shows at each desktop composition, read back with DXGI Desktop Duplication.
// usage: frame_dup SECONDS X Y OUT.csv   (X, Y: screen pixel of the game's client top-left; the game runs --frame-marker)
// The game draws 14 squares of 8x8 at its top-left: white, black, then bits 0-11 of its Present number (white = 1).
// Each desktop update with a new image gives a line: qpc of the update (LastPresentTime), qpc now, the frame number read
// (-1: no marker there), the white and black references' luminance.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char **argv)
{
    if (argc < 5) { printf("usage: frame_dup SECONDS X Y OUT.csv\n"); return 1; }
    SetProcessDPIAware();
    int secs = atoi(argv[1]), X = atoi(argv[2]), Y = atoi(argv[3]);
    IDXGIFactory1 *fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    IDXGIAdapter1 *ad = nullptr, *found_ad = nullptr;
    IDXGIOutput *found_out = nullptr;
    DXGI_OUTPUT_DESC od = {};
    for (UINT a = 0; !found_out && fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput *o = nullptr;
        for (UINT i = 0; ad->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++) {
            o->GetDesc(&od);
            RECT r = od.DesktopCoordinates;
            if (X >= r.left && X < r.right && Y >= r.top && Y < r.bottom) { found_out = o; found_ad = ad; break; }
            o->Release();
        }
        if (!found_out) ad->Release();
    }
    if (!found_out) { printf("no output at %d,%d\n", X, Y); return 1; }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(found_ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice %08lx\n", hr); return 1; }
    IDXGIOutput1 *o1 = nullptr;
    found_out->QueryInterface(__uuidof(IDXGIOutput1), (void **)&o1);
    IDXGIOutputDuplication *dup = nullptr;
    hr = o1->DuplicateOutput(dev, &dup);
    if (FAILED(hr)) { printf("DuplicateOutput %08lx\n", hr); return 1; }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = 112; sd.Height = 8; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &stage);
    int lx = X - od.DesktopCoordinates.left, ly = Y - od.DesktopCoordinates.top;
    FILE *f = fopen(argv[4], "w");
    fprintf(f, "update_qpc,now_qpc,frame,white,black\n");
    LARGE_INTEGER fq, t0, now;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    int lines = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart > (LONGLONG)secs * fq.QuadPart) break;
        DXGI_OUTDUPL_FRAME_INFO fi = {};
        IDXGIResource *res = nullptr;
        hr = dup->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { printf("AcquireNextFrame %08lx\n", hr); break; }
        if (fi.LastPresentTime.QuadPart) {
            ID3D11Texture2D *tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
            D3D11_BOX box = {(UINT)lx, (UINT)ly, 0, (UINT)lx + 112, (UINT)ly + 8, 1};
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m = {};
            int frame = -1, w = 0, b = 0;
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                auto lum = [&](int i) {
                    const unsigned char *p = (const unsigned char *)m.pData + 4 * m.RowPitch + (i * 8 + 4) * 4;
                    return (p[0] + p[1] + p[2]) / 3;
                };
                w = lum(0), b = lum(1);
                if (w > 160 && b < 90) {
                    int th = (w + b) / 2;
                    frame = 0;
                    for (int i = 0; i < 12; i++)
                        if (lum(i + 2) > th) frame |= 1 << i;
                }
                ctx->Unmap(stage, 0);
            }
            fprintf(f, "%lld,%lld,%d,%d,%d\n", fi.LastPresentTime.QuadPart, now.QuadPart, frame, w, b);
            lines++;
            tex->Release();
        }
        res->Release();
        dup->ReleaseFrame();
    }
    fclose(f);
    printf("frame_dup: %d updates written, output %ls %ldx%ld\n", lines, od.DeviceName,
           od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    return 0;
}
