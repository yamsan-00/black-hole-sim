# **black**_**hole**

A realtime GPU ray-traced black hole simulation — Schwarzschild geodesics, gravitational lensing, a swirling relativistic accretion disk, photon streams and an interactive spacetime grid.

## **Features**

- **GPU ray tracing** — every pixel integrates a null geodesic through Schwarzschild spacetime with an RK4 integrator running in an OpenGL 4.3 compute shader (`geodesic.comp`)
- **Gravitational lensing** — the starfield warps around the event horizon; escaping rays terminate early via an escape radius
- **Event horizon** — near-round capture surface with a subtle scattered edge and ember speckles, plus a smooth crimson singularity core and rim glow
- **Photon ring & corona** — smooth Gaussian photon ring at the 1.5·rₛ photon sphere and a corona that fades continuously outward from the silhouette
- **Accretion disk** — plane-crossing interpolation, soft rims, Keplerian differential rotation with trailing spiral arms, drifting fine rings, and **relativistic Doppler beaming** (the side rotating toward you is brighter)
- **Photon particle stream** — hundreds of photons continuously falling in from all directions, bent in real time (press `P`)
- **N-body star gravity** — companion stars orbit the hole on Keplerian orbits with a time-accelerated integrator (press `G` to freeze)
- **Spacetime grid** — CPU-warped grid showing the "trapdoor in spacetime"
- **Adaptive quality** — step size shrinks near the hole and grows far away; resolution and step budget drop while the camera moves; idle frames are cached and cost nothing
- **Window resize** — the compute target follows the framebuffer

## **Controls**

| Input | Action |
|---|---|
| Left-drag | Orbit the camera |
| Scroll | Zoom |
| `G` | Toggle star gravity (sticky) |
| Right-click (hold) | Momentary gravity |
| `P` | Toggle the photon particle stream |
| `O` | Toggle cinematic auto-orbit |
| Resize window | Re-renders at the new resolution |

## **Building Requirements**

1. C++ compiler supporting C++17 or newer
2. [CMake](https://cmake.org/) ≥ 3.21
3. [Vcpkg](https://vcpkg.io/en/)
4. [Git](https://git-scm.com/)
5. OpenGL 4.3-capable GPU/driver

Dependencies (`glfw3`, `glew`, `glm`) are declared in `vcpkg.json`.

## **Build Instructions**

1. Clone the repository:
   ```
   git clone https://github.com/yamsan-00/black-hole-sim.git
   cd black-hole-sim
   ```
2. Install dependencies with vcpkg:
   ```
   vcpkg install
   ```
3. Get the vcpkg toolchain path:
   ```
   vcpkg integrate install
   ```
   It prints something like: `-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`
4. Configure and build:
   ```
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build build
   ```
5. Run — executables land in `build/Debug/`:
   ```
   .\build\Debug\BlackHole3D.exe
   ```

> **Windows one-liner** (if the tools are on PATH):
> ```
> cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake && cmake --build build
> ```

### Alternative: Debian/Ubuntu apt workaround

```bash
sudo apt update
sudo apt install build-essential cmake \
	libglew-dev libglfw3-dev libglm-dev libgl1-mesa-dev
```

Then run the `cmake -B build -S .` and `cmake --build build` steps above (no toolchain file needed).

## **How the code works**

**3D (`black_hole.cpp` + `geodesic.comp`)** — the CPU side owns the window, camera, star N-body integration and uploads all state (camera basis, disk radii + animation clock, object list) to the GPU via std140 UBOs on every rendered frame (idle frames are cached and skip this). The compute shader then traces one camera ray per invocation: it converts the ray to spherical coordinates, integrates the Schwarzschild null geodesic with RK4 (adaptive step, finer inside 3·rₛ and near the disk plane), and shades whichever is hit first — the jittered horizon, the equatorial disk (with spiral/beaming shading), a scene object (segment–sphere test), or the starfield with photon ring/corona glow from the ray's closest approach. The result is written to an RGBA8 image and blitted fullscreen.

**2D (`2D_lensing.cpp`)** — a standalone CPU ray-tracer: light rays integrated around a Schwarzschild potential with RK4, drawing lensed trails in an immediate-mode OpenGL window.

**Other files** — `CPU-geodesic.cpp` and `ray_tracing.cpp` are earlier CPU-only experiments kept for reference; `grid.vert`/`grid.frag` are legacy copies of the (now inlined) grid shaders.

Performance notes: adaptive stepping, resolution drop while moving, and dirty-frame caching keep it realtime on modest GPUs; tune `maxSteps`, the `0.02·r` near-field step, and the low-res scale in `onResize` if needed.

Tested on Windows with an Intel iGPU (OpenGL 4.3). Should run anywhere with GL 4.3 compute support.

## **Mathematical Model**

### Schwarzschild metric

The spacetime around a non-rotating, uncharged black hole of mass $M$ is the Schwarzschild solution:

$$
ds^2 = f(r)\,c^2 dt^2 - f(r)^{-1} dr^2 - r^2\left(d\theta^2 + \sin^2\theta\, d\varphi^2\right),
\qquad f(r) = 1 - \frac{r_s}{r}
$$

with the **Schwarzschild radius** (event horizon)

$$
r_s = \frac{2GM}{c^2}
$$

For Sagittarius A* ($M \approx 8.54\times10^{36}\,\text{kg} \approx 4.3$ million $M_\odot$): $r_s \approx 1.27\times10^{10}\,\text{m}$ — this is the `bhRs` uniform uploaded to the shader.

### Null geodesics

Light follows null paths, $ds^2 = 0$. Because the metric is static and spherically symmetric, two quantities are conserved along a ray (affine parameter $\lambda$):

$$
E = f\,\frac{dt}{d\lambda}, \qquad L = r^2 \sin^2\theta\, \frac{d\varphi}{d\lambda}
$$

The $t$-equation follows from the null condition:

$$
\frac{dt}{d\lambda} = \sqrt{\ \frac{1}{f}\left[\frac{\dot r^2}{f} + r^2\left(\dot\theta^2 + \sin^2\theta\,\dot\varphi^2\right)\right]\ }
$$

(dots denote $d/d\lambda$). This is `initRay()`'s seeding of `E`, and `E/f` recovers $\dot t$ anywhere along the path.

### Geodesic equations of motion

The shader integrates the spatial geodesic equations in spherical coordinates (implemented in `geodesicRHS`):

$$
\ddot r = -\frac{r_s}{2r^2}\,f\,\dot t^2 + \frac{r_s}{2r^2 f}\,\dot r^2 + r f\left(\dot\theta^2 + \sin^2\theta\,\dot\varphi^2\right)
$$

$$
\ddot\theta = -\frac{2\dot r \dot\theta}{r} + \sin\theta\cos\theta\,\dot\varphi^2
$$

$$
\ddot\varphi = -\frac{2\dot r \dot\varphi}{r} - 2\cot\theta\,\dot\theta\dot\varphi
$$

The radial equation carries the physics: $-\tfrac{r_s}{2r^2}f\dot t^2$ is gravity pulling the ray inward, $\tfrac{r_s}{2r^2f}\dot r^2$ is the curvature correction, and $rf(\dots)$ is the centrifugal-like angular term. Integration is classical **RK4**:

$$
y_{n+1} = y_n + \frac{h}{6}\left(k_1 + 2k_2 + 2k_3 + k_4\right)
$$

with an **adaptive step** $h = 0.1\,r$, refined to $h = 0.02\,r$ inside $3r_s$ and $h = 0.06\,r_{\text{in}}$ near the disk plane — fine where curvature is strong, coarse where it is weak.

### Event horizon & photon sphere

A ray is captured when $r \le r_s^{\text{eff}}$, with a small hash-noise jitter $r_s^{\text{eff}} \approx r_s\,(1.00 \pm 0.03)$ for the slightly scattered silhouette. Unstable circular photon orbits exist at the **photon sphere**:

$$
r_{\text{ph}} = \frac{3}{2}\,r_s
$$

Rays whose closest approach $r_{\min}$ lands near $r_{\text{ph}}$ loop around the hole and produce the bright **photon ring**; the rendered ring intensity is a Gaussian in $r_{\min}/r_s - 1.5$. The **corona** is likewise a Gaussian in $r_{\min}/r_s - 1$, brightest at the shadow edge and fading smoothly outward.

### Accretion disk

The disk lives in the equatorial plane ($\theta = \pi/2$) between $r_{\text{in}} = 3r_s$ and $r_{\text{out}} = 6r_s$ — its inner edge sits exactly at the ISCO, which for a Schwarzschild hole is $r_{\text{ISCO}} = 6GM/c^2 = 3r_s$. Each ray's exact crossing of $y = 0$ is found by linear interpolation between integrator steps:

$$
t^* = \frac{y_{\text{prev}}}{y_{\text{prev}} - y_{\text{cur}}}, \qquad
\vec P = (1-t^*)\,\vec P_{\text{prev}} + t^*\,\vec P_{\text{cur}}
$$

Disk gas follows **Keplerian** orbits with differential rotation:

$$
\Omega(r) \propto r^{-3/2}, \qquad v_{\text{orb}} = \sqrt{\frac{GM}{r}}
$$

Visual spiral arms are $\sin\!\left(3\varphi - 3\,\Omega t - 9\,r_n\right)$ with $r_n$ the normalized radius. **Relativistic beaming** brightens the side of the disk moving toward the camera — true beaming scales intensity as $\delta^3$ with Doppler factor $\delta$; the shader uses the approximation

$$
\delta \approx 1 + 0.6\,\hat v_{\text{tan}} \cdot \hat v_{\text{toCam}}, \qquad I \propto \delta^2
$$

### N-body star gravity

Companion stars integrate the pairwise Newtonian acceleration (semi-implicit Euler, time-accelerated ×800):

$$
\vec a_i = \sum_{j \neq i} \frac{G m_j}{|\vec r_{ij}|^3}\,\vec r_{ij},
\qquad
\vec v_{i} \to \vec v_{i} + \vec a_i\,\Delta t,\quad
\vec r_{i} \to \vec r_{i} + \vec v_i\,\Delta t
$$

Initial velocities are set to circular Keplerian orbits, $v = \sqrt{GM/r}$ (≈ $3.8\times10^7\,\text{m/s}$ at $r = 4\times10^{11}\,$m), so the stars orbit stably instead of falling in. The spacetime grid displaces each vertex vertically by a Flamm-paraboloid-inspired profile:

$$
\Delta y \approx 2\sqrt{r_s\,(d - r_s)}, \qquad d = \text{distance to the mass}
$$

### Ray–camera model

Each pixel spawns a ray from the camera position $\vec C$ through a pinhole basis $(\hat r, \hat u, \hat f)$:

$$
\vec d = \text{normalize}\!\left(u\,\hat r - v\,\hat u + \hat f\right), \qquad
u = \left(\tfrac{2(p_x+0.5)}{W}-1\right)\,\alpha\,\tan\tfrac{\text{FOV}}{2}
$$

with aspect ratio $\alpha$ and a vertical flip on $v$ — the same ray setup any raster pinhole camera uses, feeding the geodesic integrator instead of a triangle rasterizer.
