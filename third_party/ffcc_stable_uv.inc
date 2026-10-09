    // ffcc patch: stable texture mapping (FfccStableUV). The first statements of GPU3D::SubmitPolygon.
    //
    // A textured quad whose texture coordinates are not an affine function of its position (the game's ground tiles
    // sometimes are: (0,0) (0,128) (64,128) (64,64) texels on a flat tile) cannot be mapped by one perspective
    // transform, so every rasteriser bends it its own way (the DS walks edges and spans, a GL renderer splits it into
    // triangles) and the bend changes with the view: the texture swims as the camera moves. Such a quad is cut into a
    // grid of small quads whose corners are the bilinear blend of the four corners (in clip space, which is exact for
    // the bilinear patch in 3D, and in texture space): each small quad is nearly affine, so every rasteriser agrees,
    // and the texture stays on the surface.
    if (NumPolygons == 0) ffcc_subdivided = 0;  // (a new scene)
    if (FfccStableUV && !ffcc_subdividing && (PolygonMode & 0x1) && ((TexParam >> 26) & 0x7) != 0 && ((TexParam >> 26) & 0x7) != 1 && ((TexParam >> 26) & 0x7) != 6 &&
        ((CurPolygonAttr >> 16) & 0x1F) == 31 && ((CurPolygonAttr >> 4) & 0x3) != 3 &&
        NumPolygons < 1200 && NumVertices < 3600 && ffcc_subdivided < 48)
    {
        const Vertex* q = TempVertexBuffer;
        bool outside = false;
        for (int c = 0; c < 3 && !outside; c++)
        {
            bool hi = true, lo = true;
            for (int i = 0; i < 4; i++)
            {
                hi = hi && q[i].Position[c] > q[i].Position[3];
                lo = lo && q[i].Position[c] < -q[i].Position[3];
            }
            outside = hi || lo;
        }
        // Texture coordinates as linear functions of (x, y, w) in clip space (a plane's perspective mapping), fitted
        // through the first three corners and checked on the fourth.
        bool inconsistent = false;
        // Only a flat parallelogram in a perspective scene (a ground tile): other quads (the bent parts of a character,
        // a quad with two corners together, the title's orthographic wave) are drawn the way the console does.
        // (Only a perspective scene: the title's orthographic wave and the menus are drawn the way the console does.)
        const bool tile = !outside && !(ProjMatrix[11] == 0 && ProjMatrix[15] == 0x1000);
        if (tile)
        {
            const double m[3][3] = {{(double)q[0].Position[0], (double)q[0].Position[1], (double)q[0].Position[3]},
                                    {(double)q[1].Position[0], (double)q[1].Position[1], (double)q[1].Position[3]},
                                    {(double)q[2].Position[0], (double)q[2].Position[1], (double)q[2].Position[3]}};
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            const double scale = std::fabs(m[0][0]) + std::fabs(m[0][1]) + std::fabs(m[0][2]) + 1.0;
            if (std::fabs(det) > 1e-6 * scale * scale * scale)
            {
                // Cramer's rule for the 4th corner's coordinates in terms of the first three: x4 = sum(l_i * m_i).
                const double p[3] = {(double)q[3].Position[0], (double)q[3].Position[1], (double)q[3].Position[3]};
                double l[3];
                for (int k = 0; k < 3; k++)
                {
                    double t[3][3];
                    for (int r = 0; r < 3; r++)
                        for (int c = 0; c < 3; c++) t[r][c] = (c == k) ? p[r] : m[c][r];
                    // (column c of the matrix holds corner c's (x, y, w))
                    const double d = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) -
                                     t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                                     t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
                    l[k] = d / det;
                }
                for (int a = 0; a < 2; a++)
                {
                    const double pred = l[0] * q[0].TexCoords[a] + l[1] * q[1].TexCoords[a] + l[2] * q[2].TexCoords[a];
                    if (std::fabs(pred - q[3].TexCoords[a]) > 8.0) inconsistent = true;
                }
            }
        }
        if (inconsistent && tile)
        {
            Vertex saved[4];
            for (int i = 0; i < 4; i++) saved[i] = TempVertexBuffer[i];
            Vertex grid[FFCC_UV_GRID + 1][FFCC_UV_GRID + 1];
            for (int j = 0; j <= FFCC_UV_GRID; j++)
            {
                for (int i = 0; i <= FFCC_UV_GRID; i++)
                {
                    const double p = (double)i / FFCC_UV_GRID, r = (double)j / FFCC_UV_GRID;
                    const double wa = (1 - p) * (1 - r), wb = p * (1 - r), wc = p * r, wd = (1 - p) * r;
                    Vertex v = saved[0];
                    for (int k = 0; k < 4; k++)
                        v.Position[k] = (s32)std::lround(wa * saved[0].Position[k] + wb * saved[1].Position[k] +
                                                         wc * saved[2].Position[k] + wd * saved[3].Position[k]);
                    for (int k = 0; k < 3; k++)
                        v.Color[k] = (s32)std::lround(wa * saved[0].Color[k] + wb * saved[1].Color[k] +
                                                      wc * saved[2].Color[k] + wd * saved[3].Color[k]);
                    for (int k = 0; k < 2; k++)
                        v.TexCoords[k] = (s16)std::lround(wa * saved[0].TexCoords[k] + wb * saved[1].TexCoords[k] +
                                                          wc * saved[2].TexCoords[k] + wd * saved[3].TexCoords[k]);
                    v.Clipped = false;
                    grid[j][i] = v;
                }
            }
            // The outline stays the original four corners (the neighbouring tiles share those edges: pieces along
            // the edge would leave cracks); the inside is a grid, and the ring between them is fanned from the corners.
            const int N = FFCC_UV_GRID;
            const u32 savedMode = PolygonMode;
            const auto submit = [&](int count, const int (*ij)[2])
            {
                for (int k = 0; k < count; k++) TempVertexBuffer[k] = grid[ij[k][1]][ij[k][0]];
                PolygonMode = count == 4 ? 1 : 0;
                LastStripPolygon = nullptr;
                SubmitPolygon();
            };
            ffcc_subdividing = true;
            ffcc_subdivided++;
            for (int j = 1; j < N - 1; j++)
            {
                for (int i = 1; i < N - 1; i++)
                {
                    const int f[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
                    submit(4, f);
                }
            }
            const int corner[4][2] = {{0, 0}, {N, 0}, {N, N}, {0, N}};
            for (int side = 0; side < 4; side++)
            {
                const int* a = corner[side];
                const int* b = corner[(side + 1) % 4];
                int pts[16][2];
                int n = 0;
                for (int k = N - 1; k >= 1; k--)
                {
                    switch (side)
                    {
                    case 0: pts[n][0] = k; pts[n][1] = 1; break;
                    case 1: pts[n][0] = N - 1; pts[n][1] = k; break;
                    case 2: pts[n][0] = N - k; pts[n][1] = N - 1; break;
                    default: pts[n][0] = 1; pts[n][1] = N - k; break;
                    }
                    n++;
                }
                // (the points run from next to b to next to a, as the sides run round the corners)
                {
                    const int f[3][2] = {{a[0], a[1]}, {b[0], b[1]}, {pts[0][0], pts[0][1]}};
                    submit(3, f);
                }
                for (int k = 0; k + 1 < n; k++)
                {
                    const int f[3][2] = {{a[0], a[1]}, {pts[k][0], pts[k][1]}, {pts[k + 1][0], pts[k + 1][1]}};
                    submit(3, f);
                }
            }
            PolygonMode = savedMode;
            ffcc_subdividing = false;
            for (int i = 0; i < 4; i++) TempVertexBuffer[i] = saved[i];
            LastStripPolygon = nullptr;
            return;
        }
    }

