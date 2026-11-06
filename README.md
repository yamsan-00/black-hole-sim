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

**3D (`black_hole.cpp` + `geodesic.comp`)** — the CPU side owns the window, camera, star N-body integration and uploads all state (camera basis, disk radii + animation clock, object list) to the GPU via std140 UBOs once per frame. The compute shader then traces one camera ray per invocation: it converts the ray to spherical coordinates, integrates the Schwarzschild null geodesic with RK4 (adaptive step, finer inside 3·rₛ and near the disk plane), and shades whichever is hit first — the jittered horizon, the equatorial disk (with spiral/beaming shading), a scene object (segment–sphere test), or the starfield with photon ring/corona glow from the ray's closest approach. The result is written to an RGBA8 image and blitted fullscreen.

**2D (`2D_lensing.cpp`)** — a standalone CPU ray-tracer: light rays integrated around a Schwarzschild potential with RK4, drawing lensed trails in an immediate-mode OpenGL window.

**Other files** — `CPU-geodesic.cpp` and `ray_tracing.cpp` are earlier CPU-only experiments kept for reference; `grid.vert`/`grid.frag` are legacy copies of the (now inlined) grid shaders.

Performance notes: adaptive stepping, resolution drop while moving, and dirty-frame caching keep it realtime on modest GPUs; tune `maxSteps`, the `0.02·r` near-field step, and the low-res scale in `onResize` if needed.

Tested on Windows with an Intel iGPU (OpenGL 4.3). Should run anywhere with GL 4.3 compute support.
