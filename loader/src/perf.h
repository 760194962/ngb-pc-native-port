// Performance recorder (--perf-at).
//
// While recording: per-frame timings and counters on the game's render thread (the thread that calls
// D3DDevice_Swap), a sampling profiler of that thread (~1 kHz, from a thread off the game CPU) and the CPU
// time of every thread. Stopping writes perf\<date_time>\ next to black.exe: summary.txt (where the frame
// time goes, what grows in slow frames, hot functions), frames.csv and threads.csv.
#pragma once

#include <cstdint>

namespace perf {

// Exclusive timing categories inside a frame (a nested scope pauses its parent).
enum Cat : int {
    kDraw,    // HLE draw calls: state translation, D3D9 calls into the driver
    kConvert, // vertex conversion for vertex-program draws, pre-transformed vertex scaling, index scans
    kTexture, // texture cache checks (sampled hashes) and uploads
    kCompile, // HLSL shader compiles (new vertex/pixel shader combinations)
    kRtCopy,  // render target copies for draws that sample a render target
    kCatCount
};

extern volatile bool g_on; // recording (cheap test for the hot paths)

void toggle();             // start or stop (any thread; applied by the render thread at the next frame)
bool recording();

// Render thread, from D3DDevice_Swap.
void swap_begin();                              // the frame's work is done
void pace_end(uint32_t vblanks, uint32_t interval); // after waiting for the vblank: vblanks since the last flip
void frame_end(uint32_t render_w, uint32_t render_h); // after Present: the next frame's work starts

// Counters (any thread).
void add_draw(uint32_t vertices, uint32_t bytes);
void add_texture_upload(uint32_t bytes);
void add_compile();
void add_rt_copy();
void add_file_read(uint32_t bytes); // game file reads (kernel NtReadFile)
void add_shader_build(double ms, bool from_cache); // one cached_compile (d3d/shader_cache.cpp): its time
// Always on, recording or not: a frame over 50 ms is logged as "[hitch]" with what that frame did (shader compiles,
// texture uploads, render target copies, file reads), so a stutter the player meets shows in black.log.

// --hitch-detail (tests): a [hitch] line also says where the slow frame's time went -- the categories above, plus
// these parts of them, timed only with the switch (off: one test of g_detail, no clock read).
enum Det : int {
    kDetTexCreate,    // a texture's system-memory copy made (CreateTexture, D3DPOOL_SYSTEMMEM)
    kDetTexFill,      // the Xbox data decoded into it (unswizzle, palette, DXT copy: our CPU work)
    kDetTexPush,      // its DEFAULT copy made and UpdateTexture (the driver's upload)
    kDetShaderCreate, // CreatePixelShader / CreateVertexShader of a shader no prewarm made (the driver's)
    kDetDrawCall,     // the host's Draw*Primitive* calls (the driver: state validation, a pipeline it makes on first use)
    kDetD3DCompile,   // D3DCompile itself (a source no cache entry and no seed had)
    kDetRingLock,     // the draw ring buffers' appending Lock + copy (D3DLOCK_NOOVERWRITE)
    kDetRingDiscard,  // the same when a ring starts over (D3DLOCK_DISCARD: the driver renames or waits for the GPU)
    kDetBind,         // SetPixelShader / SetVertexShader of another shader (D3D9On12 may make its pipeline here)
    kDetSetTexture,   // SetTexture of another texture
    kDetConstants,    // the shader constants sent with a draw
    kDetStates,       // the render states sent with a draw (apply_render_states)
    kDetPace,         // D3DDevice_Swap's frame pacing wait
    kDetPresent,      // the picture to the window (resolve, gamma, Present)
    kDetCount
};
extern volatile bool g_detail;
int64_t det_now();
void det_add(Det d, int64_t ticks);
void det_count_discard(); // a ring buffer started over this frame
struct DetScope {
    Det d;
    int64_t t;
    explicit DetScope(Det d_) : d(d_), t(g_detail ? det_now() : 0) {}
    ~DetScope()
    {
        if (t) det_add(d, det_now() - t);
    }
};

// The report finished writing: the window shows it (host.cpp, WM_APP_PERF).
const char *last_report_text();
const wchar_t *last_report_dir();
constexpr unsigned WM_APP_PERF = 0x8000 + 0x51; // WM_APP + 0x51

struct Scope {
    explicit Scope(Cat c);
    ~Scope();
    void pause(int64_t now);
    Cat cat;
    bool on;
    int64_t start = 0;
    Scope *parent = nullptr;
};

} // namespace perf
