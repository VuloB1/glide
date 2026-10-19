// Glide logo: dark rounded square with a thick ring (gap at the upper right) in a blue->cyan gradient. Drawn at any size.
// Needs <windows.h> and <gdiplus.h> included first (and `using namespace Gdiplus`).
static void DrawLogo(Graphics& g, float x, float y, float size) {
    GraphicsState st = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.TranslateTransform(x, y); g.ScaleTransform(size / 128, size / 128);
    {
        GraphicsPath bg; float d = 56;
        bg.AddArc(0.f, 0.f, d, d, 180, 90); bg.AddArc(128 - d, 0.f, d, d, 270, 90);
        bg.AddArc(128 - d, 128 - d, d, d, 0, 90); bg.AddArc(0.f, 128 - d, d, d, 90, 90); bg.CloseFigure();
        SolidBrush fill(Color(18, 20, 24)); g.FillPath(&fill, &bg);
        Pen edge(Color(44, 48, 58), 1); g.DrawPath(&edge, &bg);
    }
    g.TranslateTransform(-4, 0);
    GraphicsPath arc; arc.AddArc(34.f, 34.f, 60.f, 60.f, -40.f, -320.f);
    LinearGradientBrush gb(PointF(34, 34), PointF(94, 94), Color(91, 140, 255), Color(79, 209, 197));
    Pen pg(&gb, 11); pg.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    g.DrawPath(&pg, &arc);
    g.Restore(st);
}
