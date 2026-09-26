using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

public sealed class GlDisplayRt
{
    public int X, Y, W, H;
    public int Margin;
    public uint Tex, Fbo, Depth;
    /// <summary>Right-eye companion (stereo only); 0 when absent.</summary>
    public uint TexR, FboR, DepthR;
    public bool Dirty;
    public bool HasWideWorld;
    public long Stamp;
    public long LastDrawFrame;

    public int Wide1x => W + Margin * 2;
    public int TexW => Wide1x * GlVram.Scale;
    public int TexH => H * GlVram.Scale;

    public bool Contains(int cx0, int cy0, int cx1, int cy1)
        => cx0 >= X && cx1 <= X + W - 1 && cy0 >= Y && cy1 <= Y + H - 1;

    public bool Covers(int cx0, int cy0, int cx1, int cy1)
        => cx0 <= X && cx1 >= X + W - 1 && cy0 <= Y && cy1 >= Y + H - 1;

    public bool Intersects(int rx, int ry, int rw, int rh)
        => rx < X + W && X < rx + rw && ry < Y + H && Y < ry + rh;

    public void Create(GL gl) => (Tex, Fbo, Depth) = CreateTarget(gl);

    public void CreateStereo(GL gl)
    {
        if (FboR == 0) (TexR, FboR, DepthR) = CreateTarget(gl);
    }

    unsafe (uint tex, uint fbo, uint depthBuffer) CreateTarget(GL gl)
    {
        uint tex = gl.GenTexture();
        gl.BindTexture(TextureTarget.Texture2D, tex);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        gl.TexImage2D(TextureTarget.Texture2D, 0, InternalFormat.Rgba8, (uint)TexW, (uint)TexH, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, null);

        uint fbo = gl.GenFramebuffer();
        gl.BindFramebuffer(FramebufferTarget.Framebuffer, fbo);
        gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, tex, 0);
        uint depthBuffer = gl.GenRenderbuffer();
        gl.BindRenderbuffer(RenderbufferTarget.Renderbuffer, depthBuffer);
        gl.RenderbufferStorage(RenderbufferTarget.Renderbuffer, InternalFormat.DepthComponent24,
            (uint)TexW, (uint)TexH);
        gl.FramebufferRenderbuffer(FramebufferTarget.Framebuffer, FramebufferAttachment.DepthAttachment,
            RenderbufferTarget.Renderbuffer, depthBuffer);
        gl.ClearColor(0f, 0f, 0f, 0f);
        gl.Disable(EnableCap.ScissorTest);
        gl.Clear(ClearBufferMask.ColorBufferBit);
        gl.DepthMask(true);
        float depth = 1f;
        gl.ClearBuffer(GLEnum.Depth, 0, &depth);
        return (tex, fbo, depthBuffer);
    }

    public void Destroy(GL gl)
    {
        if (Fbo != 0) gl.DeleteFramebuffer(Fbo);
        if (Tex != 0) gl.DeleteTexture(Tex);
        if (Depth != 0) gl.DeleteRenderbuffer(Depth);
        if (FboR != 0) gl.DeleteFramebuffer(FboR);
        if (TexR != 0) gl.DeleteTexture(TexR);
        if (DepthR != 0) gl.DeleteRenderbuffer(DepthR);
        Fbo = Tex = Depth = 0;
        FboR = TexR = DepthR = 0;
    }
}
