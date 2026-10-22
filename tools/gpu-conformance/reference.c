/* Render GPU conformance scenes with the pinned Mesa llvmpipe driver and
 * compare captured frames with the result.
 *
 * Usage: reference LAUNCH-FILE [FRAMES-OUT]
 *
 * LAUNCH-FILE is written by tools/gpu-conformance.py. Each frame carries the
 * canonical device input that the board receives, the per-triangle tolerance
 * classes and edge-tie map computed by the plan, and an optional capture. The
 * process renders every frame with OpenGL 4.5 on surfaceless EGL, then
 * compares the capture in one pass over the frame in memory. It prints one
 * JSON line per comparison. FRAMES-OUT, when given, receives each unmutated
 * reference frame as width x height RGBA8 bytes, in frame order.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GLF(X) \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLCLIPCONTROLPROC, glClipControl) X(PFNGLVIEWPORTINDEXEDFPROC, glViewportIndexedf) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv) X(PFNGLCLEARBUFFERUIVPROC, glClearBufferuiv) \
    X(PFNGLCLEARBUFFERFIPROC, glClearBufferfi) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1UIPROC, glUniform1ui) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLTEXPARAMETERFVPROC, glTexParameterfv) X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc) X(PFNGLSTENCILOPPROC, glStencilOp) \
    X(PFNGLSTENCILMASKPROC, glStencilMask) X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) X(PFNGLCOLORMASKPROC, glColorMask) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLFINISHPROC, glFinish) X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)
#define DECLARE(type, name) static type p_##name;
GLF(DECLARE)
#undef DECLARE

static void die(const char *what)
{
    fprintf(stderr, "reference: %s\n", what);
    exit(2);
}

/* Mutations of the reference renderer, used only by the host self-check. */
enum { M_VIEWPORT = 1, M_FILTER = 2, M_INTERP = 4, M_MIRROR = 8, M_SRGB = 16 };

struct frame {
    uint32_t profile, width, height, stride, state, tw, th, nvert, nidx;
    uint32_t input_len, nclass, linear_tol, mutations, capture_mode;
    const uint8_t *input, *classes, *ties, *capture;
};

static const uint8_t *cursor, *end;
static uint32_t take32(void)
{
    if (end - cursor < 4) die("short launch file");
    uint32_t v;
    memcpy(&v, cursor, 4);
    cursor += 4;
    return v;
}
static const uint8_t *take(size_t n)
{
    if ((size_t)(end - cursor) < n) die("short launch file");
    const uint8_t *p = cursor;
    cursor += n;
    return p;
}

static GLuint shader(GLenum kind, const char *source)
{
    GLuint s = p_glCreateShader(kind);
    GLint ok = 0;
    p_glShaderSource(s, 1, &source, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", log);
        die("shader compilation");
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = p_glCreateProgram();
    GLint ok = 0;
    p_glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    p_glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) die("program link");
    return p;
}

/* Programs indexed by [kind][interpolation]: kind 0 colour, 1 texture, 2 id;
 * interpolation 0 noperspective, 1 smooth. */
static GLuint programs[3][2];

static void build_programs(void)
{
    static const char *qual[2] = {"noperspective", "smooth"};
    for (int q = 0; q < 2; ++q) {
        char vs[1024], fs[1024];
        snprintf(vs, sizeof vs,
                 "#version 450 core\nlayout(location=0) in vec4 p; layout(location=1) in vec4 a;\n"
                 "%s out vec4 v; void main(){ v=a; gl_Position=p; }\n", qual[q]);
        snprintf(fs, sizeof fs,
                 "#version 450 core\n%s in vec4 v; layout(location=0) out vec4 o; void main(){ o=v; }\n", qual[q]);
        programs[0][q] = program(vs, fs);
        snprintf(vs, sizeof vs,
                 "#version 450 core\nlayout(location=0) in vec4 p; layout(location=1) in vec2 a;\n"
                 "%s out vec2 v; void main(){ v=a; gl_Position=p; }\n", qual[q]);
        snprintf(fs, sizeof fs,
                 "#version 450 core\nuniform sampler2D s; %s in vec2 v; layout(location=0) out vec4 o;\n"
                 "void main(){ o=textureLod(s,v,0.0); }\n", qual[q]);
        programs[1][q] = program(vs, fs);
    }
    programs[2][0] = programs[2][1] = program(
        "#version 450 core\nlayout(location=0) in vec4 p; void main(){ gl_Position=p; }\n",
        "#version 450 core\nuniform uint base; layout(location=0) out uint o;\n"
        "void main(){ o=base+uint(gl_PrimitiveID)+1u; }\n");
}

static const GLenum stencil_ops[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR,
                                      GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
static const GLenum address[5] = {GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE,
                                  GL_CLAMP_TO_BORDER, GL_MIRROR_CLAMP_TO_EDGE};

static void blend(unsigned equation, int id_pass)
{
    if (!equation || id_pass) {
        p_glDisable(GL_BLEND);
        return;
    }
    p_glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD);
    switch (equation) {
    case 1: p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    case 2: p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    case 3: p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE); break;
    default: p_glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO); break;
    }
}

/* Draw one frame into the bound framebuffer. id_pass selects the id program
 * with blending disabled; depth, stencil, culling and colour masks are the
 * same in both passes, so the id buffer names the last colour-writing
 * triangle at each pixel. */
static void draw(const struct frame *f, unsigned mutations, int id_pass)
{
    uint32_t vstride = f->profile == 1 ? 32 : 24;
    GLuint vao, vbo, ebo = 0, tex = 0;
    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)f->nvert * vstride, f->input, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)vstride, (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, f->profile == 1 ? 4 : 2, GL_FLOAT, GL_FALSE, (GLsizei)vstride, (void *)16);
    p_glEnableVertexAttribArray(1);
    int smooth = f->profile == 4;
    if (mutations & M_INTERP) smooth = !smooth;
    GLuint prog = programs[id_pass ? 2 : f->profile == 1 ? 0 : 1][smooth];
    p_glUseProgram(prog);
    GLint base_location = id_pass ? p_glGetUniformLocation(prog, "base") : -1;

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_BLEND);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glDepthMask(GL_TRUE);
    p_glStencilMask(0xff);

    if (f->profile == 1) {
        if (id_pass) p_glUniform1ui(base_location, 0);
        p_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)f->nvert);
    } else {
        uint32_t nbatch = f->profile == 4 ? (f->state & 31) : 0;
        const uint8_t *indices = f->input + (size_t)f->nvert * 24;
        const uint8_t *batches = indices + (size_t)f->nidx * 2;
        const uint8_t *texels = batches + (size_t)nbatch * 16;
        uint32_t tstride = (f->tw + 1) & ~1u;
        p_glGenBuffers(1, &ebo);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)f->nidx * 2, indices, GL_STATIC_DRAW);
        if (!id_pass) {
            unsigned frame_state = f->profile == 4 ? f->state : 0;
            int linear = (frame_state >> 5) & 1, srgb = (frame_state >> 12) & 1;
            unsigned u = f->profile == 4 ? (frame_state >> 6) & 7 : 2;
            unsigned v = f->profile == 4 ? (frame_state >> 9) & 7 : 2;
            if (mutations & M_FILTER) linear = !linear;
            if (mutations & M_SRGB) srgb = 0;
            GLenum wrap_u = address[u], wrap_v = address[v];
            if (mutations & M_MIRROR) {
                if (u == 4) wrap_u = GL_MIRRORED_REPEAT;
                if (v == 4) wrap_v = GL_MIRRORED_REPEAT;
            }
            static const GLfloat border[4] = {0, 0, 0, 0};
            p_glActiveTexture(GL_TEXTURE0);
            p_glGenTextures(1, &tex);
            p_glBindTexture(GL_TEXTURE_2D, tex);
            p_glTexStorage2D(GL_TEXTURE_2D, 1, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, (GLsizei)f->tw, (GLsizei)f->th);
            p_glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)tstride);
            p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)f->tw, (GLsizei)f->th, GL_RGBA, GL_UNSIGNED_BYTE, texels);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrap_u);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrap_v);
            p_glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
            p_glUniform1i(p_glGetUniformLocation(prog, "s"), 0);
        }
        if (f->profile != 4) {
            if (f->profile == 3 && (f->state & 3)) {
                static const GLenum depth[4] = {GL_ALWAYS, GL_LESS, GL_LEQUAL, GL_ALWAYS};
                p_glEnable(GL_DEPTH_TEST);
                p_glDepthFunc(depth[f->state & 3]);
                p_glDepthMask((f->state & 4) ? GL_TRUE : GL_FALSE);
            }
            if (f->profile == 3) blend((f->state >> 3) & 1, id_pass);
            if (id_pass) p_glUniform1ui(base_location, 0);
            p_glDrawElements(GL_TRIANGLES, (GLsizei)f->nidx, GL_UNSIGNED_SHORT, (void *)0);
        } else {
            uint32_t drawn = 0;
            for (uint32_t b = 0; b < nbatch; ++b) {
                uint32_t first, count, state, stencil;
                memcpy(&first, batches + 16 * b, 4);
                memcpy(&count, batches + 16 * b + 4, 4);
                memcpy(&state, batches + 16 * b + 8, 4);
                memcpy(&stencil, batches + 16 * b + 12, 4);
                p_glEnable(GL_DEPTH_TEST);
                p_glDepthFunc(GL_NEVER + (state & 7));
                p_glDepthMask((state & 8) ? GL_TRUE : GL_FALSE);
                unsigned cull = (state >> 4) & 3;
                if (cull) {
                    p_glEnable(GL_CULL_FACE);
                    p_glFrontFace(GL_CCW);
                    p_glCullFace(cull == 1 ? GL_FRONT : GL_BACK);
                } else {
                    p_glDisable(GL_CULL_FACE);
                }
                blend((state >> 6) & 7, id_pass);
                GLboolean write = (state & (1u << 9)) ? GL_FALSE : GL_TRUE;
                p_glColorMask(write, write, write, write);
                if (state & (1u << 10)) {
                    p_glEnable(GL_STENCIL_TEST);
                    p_glStencilFunc(GL_NEVER + ((state >> 11) & 7), (GLint)(stencil & 255), (stencil >> 8) & 255);
                    p_glStencilMask((stencil >> 16) & 255);
                    p_glStencilOp(stencil_ops[(state >> 14) & 7], stencil_ops[(state >> 17) & 7],
                                  stencil_ops[(state >> 20) & 7]);
                } else {
                    p_glDisable(GL_STENCIL_TEST);
                }
                if (id_pass) p_glUniform1ui(base_location, drawn);
                p_glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_SHORT, (void *)(uintptr_t)(first * 2u));
                drawn += count / 3;
            }
        }
    }
    p_glFinish();
    if (tex) p_glDeleteTextures(1, &tex);
    if (ebo) p_glDeleteBuffers(1, &ebo);
    p_glDeleteBuffers(1, &vbo);
    p_glDeleteVertexArrays(1, &vao);
}

/* Render one frame; returns RGBA8 pixels (width x height) and, when ids is
 * non-null, the id of the last colour-writing triangle at each pixel. */
static void render(const struct frame *f, unsigned mutations, uint8_t *rgba, uint32_t *ids)
{
    GLenum depth_format = f->profile == 3 ? GL_DEPTH_COMPONENT32F : f->profile == 4 ? GL_DEPTH32F_STENCIL8 : 0;
    GLenum depth_attachment = f->profile == 4 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    for (int pass = 0; pass < (ids ? 2 : 1); ++pass) {
        GLuint fbo, rb[2] = {0, 0};
        p_glGenFramebuffers(1, &fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glGenRenderbuffers(depth_format ? 2 : 1, rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
        p_glRenderbufferStorage(GL_RENDERBUFFER, pass ? GL_R32UI : GL_RGBA8, (GLsizei)f->width, (GLsizei)f->height);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
        if (depth_format) {
            p_glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
            p_glRenderbufferStorage(GL_RENDERBUFFER, depth_format, (GLsizei)f->width, (GLsizei)f->height);
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment, GL_RENDERBUFFER, rb[1]);
        }
        GLenum buffers[1] = {GL_COLOR_ATTACHMENT0};
        p_glDrawBuffers(1, buffers);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("incomplete framebuffer");
        p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glDepthMask(GL_TRUE);
        p_glStencilMask(0xff);
        p_glDisable(GL_SCISSOR_TEST);
        if (pass) {
            static const GLuint zero[4] = {0, 0, 0, 0};
            p_glClearBufferuiv(GL_COLOR, 0, zero);
        } else {
            /* The background program stores 0xffc08020: RGBA bytes 20 80 c0 ff. */
            static const GLfloat clear[4] = {0x20 / 255.0f, 0x80 / 255.0f, 0xc0 / 255.0f, 1.0f};
            p_glClearBufferfv(GL_COLOR, 0, clear);
        }
        if (f->profile == 3) {
            static const GLfloat one = 1.0f;
            p_glClearBufferfv(GL_DEPTH, 0, &one);
        } else if (f->profile == 4) {
            p_glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
        p_glViewportIndexedf(0, (mutations & M_VIEWPORT) ? 0.5f : 0.0f, 0.0f, (GLfloat)f->width, (GLfloat)f->height);
        draw(f, mutations, pass);
        p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        if (pass)
            p_glReadPixels(0, 0, (GLsizei)f->width, (GLsizei)f->height, GL_RED_INTEGER, GL_UNSIGNED_INT, ids);
        else
            p_glReadPixels(0, 0, (GLsizei)f->width, (GLsizei)f->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (p_glGetError() != GL_NO_ERROR) die("OpenGL error");
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteRenderbuffers(depth_format ? 2 : 1, rb);
        p_glDeleteFramebuffers(1, &fbo);
    }
}

#define POISON UINT32_C(0x73951a6e)

static unsigned tolerance(const struct frame *f, const uint32_t *ids, size_t i)
{
    uint32_t id = ids[i];
    if (!id) return 0;
    if (id > f->nclass) die("triangle id outside the class table");
    uint8_t c = f->classes[id - 1];
    return c == 0 ? 0 : c == 1 ? 1 : f->linear_tol;
}

static int within(const uint8_t *a, const uint8_t *b, unsigned tol)
{
    for (int c = 0; c < 4; ++c) {
        int d = (int)a[c] - (int)b[c];
        if (d < 0) d = -d;
        if ((unsigned)d > tol) return 0;
    }
    return 1;
}

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Compare the capture with one reference rendering, in one pass. */
static void compare(const struct frame *f, unsigned index, unsigned mutation, const uint8_t *ref,
                    const uint32_t *ids, const uint8_t *capture)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t w = f->width, h = f->height;
    uint64_t exact = 0, tolerant = 0, tie = 0, failed = 0, stale = 0, sentinel = 0, covered = 0, ties = 0;
    uint32_t list[16][4];
    unsigned listed = 0;
    uint8_t poison[4] = {POISON & 255, (POISON >> 8) & 255, (POISON >> 16) & 255, POISON >> 24};
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            const uint8_t *r = ref + 4 * i, *c = capture + 4 * ((size_t)y * f->stride + x);
            unsigned tol = tolerance(f, ids, i);
            int is_tie = f->ties && ((f->ties[i >> 3] >> (i & 7)) & 1);
            covered += ids[i] != 0;
            ties += is_tie;
            if (within(r, poison, tol)) ++sentinel;
            if (within(r, c, tol)) {
                if (!memcmp(r, c, 4)) ++exact; else ++tolerant;
                continue;
            }
            int accepted = 0;
            if (is_tie) {
                for (int dy = -1; dy <= 1 && !accepted; ++dy) {
                    for (int dx = -1; dx <= 1 && !accepted; ++dx) {
                        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        size_t n = (size_t)ny * w + (size_t)nx;
                        unsigned t = tolerance(f, ids, n);
                        if (within(ref + 4 * n, c, t > tol ? t : tol)) accepted = 1;
                    }
                }
            }
            if (accepted) {
                ++tie;
                continue;
            }
            ++failed;
            if (word(c) == POISON) ++stale;
            if (listed < 16) {
                list[listed][0] = x;
                list[listed][1] = y;
                list[listed][2] = word(r);
                list[listed][3] = word(c);
                ++listed;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("{\"frame\":%u,\"mutation\":%u,\"pixels\":%llu,\"covered\":%llu,\"ties\":%llu,\"exact\":%llu,"
           "\"tolerant\":%llu,\"tie_accepted\":%llu,\"failed\":%llu,\"stale\":%llu,\"sentinel\":%llu,"
           "\"compare_ms\":%.3f,\"failures\":[",
           index, mutation, (unsigned long long)w * h, (unsigned long long)covered, (unsigned long long)ties,
           (unsigned long long)exact, (unsigned long long)tolerant, (unsigned long long)tie,
           (unsigned long long)failed, (unsigned long long)stale, (unsigned long long)sentinel, ms);
    for (unsigned k = 0; k < listed; ++k)
        printf("%s[%u,%u,%u,%u]", k ? "," : "", list[k][0], list[k][1], list[k][2], list[k][3]);
    printf("]}\n");
}

static void load_gl(void)
{
#define LOAD(type, name) \
    if (!(p_##name = (type)eglGetProcAddress(#name))) die("missing " #name);
    GLF(LOAD)
#undef LOAD
}

/* Report which shared objects provide EGL and the driver, so the caller can
 * check that the pinned build, not a system Mesa, rendered the frames. */
static void report_libraries(void)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[4096], last[4096] = "";
    if (!maps) return;
    while (fgets(line, sizeof line, maps)) {
        char *path = strchr(line, '/');
        if (!path) continue;
        path[strcspn(path, "\n")] = 0;
        if ((strstr(path, "libEGL") || strstr(path, "libgallium") || strstr(path, "libGL")) && strcmp(path, last)) {
            printf("{\"library\":\"%s\"}\n", path);
            snprintf(last, sizeof last, "%s", path);
        }
    }
    fclose(maps);
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) die("usage: reference LAUNCH-FILE [FRAMES-OUT]");
    FILE *in = fopen(argv[1], "rb");
    if (!in) die("cannot open launch file");
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) die("cannot read launch file");
    fclose(in);
    FILE *out = argc == 3 ? fopen(argv[2], "wb") : NULL;
    if (argc == 3 && !out) die("cannot open frame output");
    cursor = data;
    end = data + size;
    if (take32() != 0x31464347) die("launch file magic");
    uint32_t count = take32();

    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) die("EGL initialization");
    if (!eglBindAPI(EGL_OPENGL_API)) die("EGL OpenGL API");
    static const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                        EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        die("EGL context");
    load_gl();
    printf("{\"gl_version\":\"%s\",\"gl_renderer\":\"%s\"}\n", (const char *)p_glGetString(GL_VERSION),
           (const char *)p_glGetString(GL_RENDERER));
    report_libraries();
    p_glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    build_programs();

    for (uint32_t index = 0; index < count; ++index) {
        struct frame f;
        f.profile = take32();
        f.width = take32();
        f.height = take32();
        f.stride = take32();
        f.state = take32();
        f.tw = take32();
        f.th = take32();
        f.nvert = take32();
        f.nidx = take32();
        f.input_len = take32();
        f.input = take(f.input_len);
        f.nclass = take32();
        f.classes = take(f.nclass);
        f.linear_tol = take32();
        f.ties = take32() ? take(((size_t)f.width * f.height + 7) / 8) : NULL;
        f.capture_mode = take32();
        f.capture = f.capture_mode == 1 ? take((size_t)f.stride * f.height * 4) : NULL;
        f.mutations = take32();
        if (f.profile < 1 || f.profile > 4 || !f.width || !f.height || f.width > 1920 || f.height > 1080 ||
            f.stride < f.width)
            die("frame geometry");
        uint32_t vstride = f.profile == 1 ? 32 : 24;
        uint32_t nbatch = f.profile == 4 ? (f.state & 31) : 0;
        size_t expected = (size_t)f.nvert * vstride;
        if (f.profile > 1)
            expected += (size_t)f.nidx * 2 + (size_t)nbatch * 16 + (size_t)((f.tw + 1) & ~1u) * f.th * 4;
        if (expected != f.input_len || !f.nvert) die("input length differs from its shape");
        if (f.profile > 1) {
            const uint8_t *ix = f.input + (size_t)f.nvert * 24;
            for (uint32_t k = 0; k < f.nidx; ++k) {
                uint16_t v;
                memcpy(&v, ix + 2 * k, 2);
                if (v >= f.nvert) die("vertex index outside the vertex array");
            }
            const uint8_t *bt = ix + (size_t)f.nidx * 2;
            for (uint32_t b = 0; b < nbatch; ++b) {
                uint32_t first, n;
                memcpy(&first, bt + 16 * b, 4);
                memcpy(&n, bt + 16 * b + 4, 4);
                if (first > f.nidx || n > f.nidx - first) die("batch outside the index buffer");
            }
        }
        size_t pixels = (size_t)f.width * f.height;
        uint8_t *ref = malloc(4 * pixels);
        uint32_t *ids = malloc(4 * pixels);
        if (!ref || !ids) die("memory");
        render(&f, 0, ref, ids);
        if (out) fwrite(ref, 4, pixels, out);
        const uint8_t *capture = f.capture;
        uint8_t *self = NULL;
        if (f.capture_mode == 2) {
            /* Compare the reference with a copy of itself, laid out at the
             * capture stride, to measure the comparator on this frame. */
            self = calloc((size_t)f.stride * f.height, 4);
            if (!self) die("memory");
            for (uint32_t y = 0; y < f.height; ++y)
                memcpy(self + 4 * (size_t)y * f.stride, ref + 4 * (size_t)y * f.width, 4 * (size_t)f.width);
            capture = self;
        }
        if (capture) {
            compare(&f, index, 0, ref, ids, capture);
            uint8_t *mutated = malloc(4 * pixels);
            if (!mutated) die("memory");
            for (unsigned bit = 1; bit <= M_SRGB; bit <<= 1) {
                if (!(f.mutations & bit)) continue;
                render(&f, bit, mutated, NULL);
                compare(&f, index, bit, mutated, ids, capture);
            }
            free(mutated);
        }
        free(self);
        free(ids);
        free(ref);
        fflush(stdout);
    }
    if (cursor != end) die("trailing launch file bytes");
    if (out) fclose(out);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
