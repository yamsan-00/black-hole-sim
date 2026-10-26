
// black_hole.cpp  (PATCHED)
//
// Fixes applied (see review notes):
//   1.  glEnable(GL_PROGRAM_POINT_SIZE) so gl_PointSize in particle VS works
//   2.  GL debug callback for high/medium severity messages
//   3.  Right-mouse "Gravity" no longer clobbers the G-key sticky toggle:
//         - Gravity      = sticky toggle (G key)
//         - GravityPull  = momentary hold (right mouse button)
//   4.  Particle initial velocity capped below c (2e8 m/s, was 1e9 m/s)
//   5.  Window resize support via framebuffer-size callback; compute texture
//       and Camera UBO resolution are updated accordingly
//   6.  Grid shaders inlined (no external grid.vert / grid.frag required)
//   7.  Particle shader build moved out of generateParticles() into
//       createParticleShader() for cleaner separation
//   8.  Objects UBO: float mass[16] -> vec4 mass[16] to match std140 layout
//       (was a latent OOB bug; mass is still unused by the shader but the
//        layout is now correct for future use)
//   9.  Disk UBO: repurposed unused disk_num slot for SagA.r_s (bhRs)
//  10.  Grid rebuild throttled to 50 ms while gravity is active
//  11.  Camera UBO carries ivec2 resolution so the compute shader doesn't
//       hardcode 800x600 / 400x300
//
// NOTE: geodesic.comp is still loaded from disk (it's the physics core and
//       benefits from being editable separately).
// =============================================================================

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <vector>
#define _USE_MATH_DEFINES
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace glm;
using namespace std;
using Clock = std::chrono::high_resolution_clock;

double c = 299792458.0;
double G = 6.67430e-11;

bool Gravity      = true;    // [ANIM] sticky toggle (G key) — on by default
bool GravityPull  = false;   // momentary    (right mouse held)
bool ShowParticles = false;
bool AutoOrbit     = true;   // [ANIM] cinematic camera drift when idle
bool ViewDirty     = true;   // [PERF] skip raytrace when nothing changed

struct Camera {
  vec3 target = vec3(0.0f);
  float radius = 8e10f;
  float minRadius = 2e10f, maxRadius = 1e12f;
  float azimuth = 0.0f;
  float elevation = M_PI / 3.0f;
  float orbitSpeed = 0.01f;
  double zoomSpeed = 25e9f;
  bool dragging = false;
  bool moving = false;
  double lastX = 0.0, lastY = 0.0;

  vec3 position() const {
    float clampedElevation = glm::clamp(elevation, 0.01f, float(M_PI) - 0.01f);
    return vec3(radius * sin(clampedElevation) * cos(azimuth),
                radius * cos(clampedElevation),
                radius * sin(clampedElevation) * sin(azimuth));
  }
    void update() {
        target = vec3(0.0f);
        moving = dragging;
        ViewDirty = true;   // [PERF]
    }
  void processMouseMove(double x, double y) {
    float dx = float(x - lastX);
    float dy = float(y - lastY);
    if (dragging) {
      azimuth += dx * orbitSpeed;
      elevation -= dy * orbitSpeed;
      elevation = glm::clamp(elevation, 0.01f, float(M_PI) - 0.01f);
    }
    lastX = x;
    lastY = y;
    update();
  }
  void processMouseButton(int button, int action, int mods, GLFWwindow *win) {
    if (button == GLFW_MOUSE_BUTTON_LEFT ||
        button == GLFW_MOUSE_BUTTON_MIDDLE) {
      if (action == GLFW_PRESS) {
        dragging = true;
        glfwGetCursorPos(win, &lastX, &lastY);
      } else if (action == GLFW_RELEASE) {
        dragging = false;
        update();   // [FIX] moving true->false triggers full-res refresh
      }
    }
    // [FIX] right mouse is now momentary only, doesn't touch Gravity
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
      if (action == GLFW_PRESS)
        GravityPull = true;
      else if (action == GLFW_RELEASE)
        GravityPull = false;
    }
  }
  void processScroll(double xoffset, double yoffset) {
    radius -= float(yoffset * zoomSpeed);
    radius = glm::clamp(radius, minRadius, maxRadius);
    update();
  }
  void processKey(int key, int scancode, int action, int mods) {
    if (action == GLFW_PRESS && key == GLFW_KEY_G) {
      Gravity = !Gravity;
      cout << "[INFO] Gravity (sticky) " << (Gravity ? "ON" : "OFF") << endl;
    }
        if (action == GLFW_PRESS && key == GLFW_KEY_P) {
          ShowParticles = !ShowParticles;
          cout << "[INFO] Particles " << (ShowParticles ? "ON" : "OFF") << endl;
        }
        if (action == GLFW_PRESS && key == GLFW_KEY_O) {
          AutoOrbit = !AutoOrbit;
          cout << "[INFO] Auto-orbit " << (AutoOrbit ? "ON" : "OFF") << endl;
        }
  }
};
Camera camera;

struct BlackHole {
  vec3 position;
  double mass;
  double r_s;
  BlackHole(vec3 pos, float m) : position(pos), mass(m) {
    r_s = 2.0 * G * mass / (c * c);
  }
};
BlackHole SagA(vec3(0.0f), 8.54e36);

struct ObjectData {
  vec4 posRadius;
  vec4 color;
  float mass;
  vec3 velocity = vec3(0.0f);
};
// [ANIM] stars start on circular Keplerian orbits: v = sqrt(GM/r)
// (GM/r)^0.5 at r=4e11 around SagA* ≈ 3.8e7 m/s
vector<ObjectData> objects = {
    { vec4(4e11f, 0.0f, 0.0f, 4e10f), vec4(1, 1, 0, 1), 1.98892e30,
      vec3(0.0f, 0.0f, -3.8e7f) },
    { vec4(0.0f, 0.0f, 4e11f, 4e10f), vec4(1, 0, 0, 1), 1.98892e30,
      vec3(3.8e7f, 0.0f, 0.0f) },
    { vec4(0.0f, 0.0f, 0.0f, float(SagA.r_s)), vec4(0, 0, 0, 1),
      float(SagA.mass) },
};

struct Engine {
  GLuint gridShaderProgram = 0;
  GLFWwindow *window = nullptr;
  GLuint quadVAO = 0, texture = 0, shaderProgram = 0, computeProgram = 0;
  GLuint cameraUBO = 0, diskUBO = 0, objectsUBO = 0;
  GLuint gridVAO = 0, gridVBO = 0, gridEBO = 0;
  GLuint particleVAO = 0, particleVBO = 0, particleProgram = 0;
  int gridIndexCount = 0;
  int particleCount = 0;

  int WIDTH = 800, HEIGHT = 600;
  int COMPUTE_WIDTH = 800, COMPUTE_HEIGHT = 600;
  int COMPUTE_WIDTH_LOW = 400, COMPUTE_HEIGHT_LOW = 300;

  Engine() {
    if (!glfwInit()) {
      cerr << "GLFW init failed\n";
      exit(EXIT_FAILURE);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE); // [FIX] debug context
    window = glfwCreateWindow(WIDTH, HEIGHT, "Black Hole", nullptr, nullptr);
    if (!window) {
      cerr << "Failed to create GLFW window\n";
      glfwTerminate();
      exit(EXIT_FAILURE);
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
      cerr << "GLEW init failed\n";
      glfwTerminate();
      exit(EXIT_FAILURE);
    }
    cout << "OpenGL " << glGetString(GL_VERSION) << "\n";

    // [FIX] GL debug callback (GL 4.3+)
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(
        [](GLenum, GLenum, GLuint, GLenum sev, GLsizei, const GLchar *msg,
           const void *) {
          if (sev == GL_DEBUG_SEVERITY_HIGH)
            cerr << "[GL HIGH] " << msg << "\n";
          else if (sev == GL_DEBUG_SEVERITY_MEDIUM)
            cerr << "[GL MED ] " << msg << "\n";
        },
        nullptr);

    // [FIX] required for gl_PointSize written in particle vertex shader
    glEnable(GL_PROGRAM_POINT_SIZE);

    shaderProgram = CreateShaderProgram(); // fullscreen quad
    gridShaderProgram =
        CreateGridShaderProgram(); // [FIX] inline (no external files)
    computeProgram = CreateComputeProgram("geodesic.comp");
    createParticleShader(); // [FIX] moved out of generateParticles

    // Camera UBO — std140 layout, 128 bytes (fits new resolution field)
    glGenBuffers(1, &cameraUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
    glBufferData(GL_UNIFORM_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, cameraUBO);

    // Disk UBO — 16 bytes: { disk_r1, disk_r2, bhRs, thickness }
    glGenBuffers(1, &diskUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, diskUBO);
    glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, diskUBO);

    // Objects UBO — std140: int + 3 pad + 3 * (16 * vec4) = 784 bytes
    // [FIX] mass promoted to vec4[16] (was float[16], caused std140 mismatch)
    glGenBuffers(1, &objectsUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
    GLsizeiptr sz = sizeof(int) + 3 * sizeof(float) +
                    16 * (sizeof(vec4) + sizeof(vec4) + sizeof(vec4));
    glBufferData(GL_UNIFORM_BUFFER, sz, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, objectsUBO);

    auto r = QuadVAO();
    quadVAO = r[0];
    texture = r[1];

    generateGrid(objects);
    generateParticles();
  }

  // [FIX] called on window resize; re-allocates the compute texture
  void onResize(int w, int h) {
    if (w <= 0 || h <= 0)
      return;
    WIDTH = w;
    HEIGHT = h;
    COMPUTE_WIDTH = w;
    COMPUTE_HEIGHT = h;
    COMPUTE_WIDTH_LOW = (w / 2 > 1) ? w / 2 : 1;
    COMPUTE_HEIGHT_LOW = (h / 2 > 1) ? h / 2 : 1;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, COMPUTE_WIDTH, COMPUTE_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ViewDirty = true;   // [PERF]
    cout << "[INFO] Resized -> " << w << "x" << h << "  (low "
         << COMPUTE_WIDTH_LOW << "x" << COMPUTE_HEIGHT_LOW << ")\n";
  }

  void generateGrid(const vector<ObjectData> &objs) {
    const int gridSize = 25;
    const float spacing = 1e10f;
    vector<vec3> verts;
    verts.reserve((gridSize + 1) * (gridSize + 1));
    vector<GLuint> idx;
    idx.reserve(gridSize * gridSize * 4);

    for (int z = 0; z <= gridSize; ++z) {
      for (int x = 0; x <= gridSize; ++x) {
        float wx = (x - gridSize / 2) * spacing;
        float wz = (z - gridSize / 2) * spacing;
        float y = 0.0f;
        for (auto &o : objs) {
          vec3 op = vec3(o.posRadius);
          double rs = 2.0 * G * o.mass / (c * c);
          double dx = wx - op.x, dz = wz - op.z;
          double d = sqrt(dx * dx + dz * dz);
          if (d > rs)
            y += float(2.0 * sqrt(rs * (d - rs))) - 3e10f;
          else
            y += 2.0f * float(sqrt(rs * rs)) - 3e10f;
        }
        verts.emplace_back(wx, y, wz);
      }
    }
    for (int z = 0; z < gridSize; ++z) {
      for (int x = 0; x < gridSize; ++x) {
        int i = z * (gridSize + 1) + x;
        idx.push_back(i);
        idx.push_back(i + 1);
        idx.push_back(i);
        idx.push_back(i + gridSize + 1);
      }
    }

    if (!gridVAO)
      glGenVertexArrays(1, &gridVAO);
    if (!gridVBO)
      glGenBuffers(1, &gridVBO);
    if (!gridEBO)
      glGenBuffers(1, &gridEBO);

    glBindVertexArray(gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(vec3), verts.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gridEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(GLuint),
                 idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3), (void *)0);
    gridIndexCount = (int)idx.size();
    glBindVertexArray(0);
  }

  void drawGrid(const mat4 &vp) {
    glUseProgram(gridShaderProgram);
    glUniformMatrix4fv(glGetUniformLocation(gridShaderProgram, "viewProj"), 1,
                       GL_FALSE, value_ptr(vp));
    glBindVertexArray(gridVAO);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
  }

  void drawFullScreenQuad() {
    glUseProgram(shaderProgram);
    glBindVertexArray(quadVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(shaderProgram, "screenTexture"), 0);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 6);
    glEnable(GL_DEPTH_TEST);
  }

  // ---------- fullscreen-quad shader (unchanged) ----------
  GLuint CreateShaderProgram() {
    const char *vs = R"(
            #version 330 core
            layout(location=0) in vec2 aPos;
            layout(location=1) in vec2 aTexCoord;
            out vec2 TexCoord;
            void main() { gl_Position = vec4(aPos, 0.0, 1.0); TexCoord = aTexCoord; }
        )";
    const char *fs = R"(
            #version 330 core
            in vec2 TexCoord; out vec4 FragColor;
            uniform sampler2D screenTexture;
            void main() { FragColor = texture(screenTexture, TexCoord); }
        )";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
  }

  // [FIX] inline grid shader (replaces external grid.vert / grid.frag)
  GLuint CreateGridShaderProgram() {
    const char *vs = R"(
            #version 330 core
            layout(location=0) in vec3 aPos;
            uniform mat4 viewProj;
            void main() { gl_Position = viewProj * vec4(aPos, 1.0); }
        )";
    const char *fs = R"(
            #version 330 core
            out vec4 FragColor;
            void main() { FragColor = vec4(0.1, 0.6, 0.9, 0.4); }
        )";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      GLint len;
      glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
      vector<char> log(len);
      glGetProgramInfoLog(p, len, nullptr, log.data());
      cerr << "Grid link error: " << log.data() << "\n";
      exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
  }

  // ---------- file-based shader loader (kept for geodesic.comp etc.)
  // ----------
  GLuint CreateShaderProgram(const char *vp, const char *fp) {
    auto load = [](const char *path, GLenum t) {
      ifstream in(path);
      if (!in.is_open()) {
        cerr << "Shader missing: " << path << "\n";
        exit(1);
      }
      stringstream ss;
      ss << in.rdbuf();
      string s = ss.str();
      const char *c = s.c_str();
      GLuint sh = glCreateShader(t);
      glShaderSource(sh, 1, &c, nullptr);
      glCompileShader(sh);
      GLint ok;
      glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
      if (!ok) {
        GLint len;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        vector<char> log(len);
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        cerr << "Shader error " << path << ": " << log.data() << "\n";
        exit(1);
      }
      return sh;
    };
    GLuint v = load(vp, GL_VERTEX_SHADER);
    GLuint f = load(fp, GL_FRAGMENT_SHADER);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      GLint len;
      glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
      vector<char> log(len);
      glGetProgramInfoLog(p, len, nullptr, log.data());
      cerr << "Link error: " << log.data() << "\n";
      exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
  }

  GLuint CreateComputeProgram(const char *path) {
    ifstream in(path);
    if (!in.is_open()) {
      cerr << "Compute missing: " << path << "\n";
      exit(1);
    }
    stringstream ss;
    ss << in.rdbuf();
    string s = ss.str();
    const char *c = s.c_str();
    GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &c, nullptr);
    glCompileShader(cs);
    GLint ok;
    glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      GLint len;
      glGetShaderiv(cs, GL_INFO_LOG_LENGTH, &len);
      vector<char> log(len);
      glGetShaderInfoLog(cs, len, nullptr, log.data());
      cerr << "Compute error: " << log.data() << "\n";
      exit(1);
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, cs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      GLint len;
      glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
      vector<char> log(len);
      glGetProgramInfoLog(p, len, nullptr, log.data());
      cerr << "Compute link: " << log.data() << "\n";
      exit(1);
    }
    glDeleteShader(cs);
    return p;
  }

  void dispatchCompute(const Camera &cam) {
    bool mov = cam.moving;
    int cw = mov ? COMPUTE_WIDTH_LOW : COMPUTE_WIDTH;
    int ch = mov ? COMPUTE_HEIGHT_LOW : COMPUTE_HEIGHT;

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);

    glUseProgram(computeProgram);
    uploadCameraUBO(cam);
    uploadDiskUBO(float(glfwGetTime()));
    uploadObjectsUBO(objects);

    glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    GLuint gx = (cw + 15) / 16;
    GLuint gy = (ch + 15) / 16;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
  }

  // [FIX] updated to carry ivec2 resolution (matches shader UBO)
  void uploadCameraUBO(const Camera &cam) {
    struct UBO {
      vec3 pos;
      float _p0; // 0
      vec3 right;
      float _p1; // 16
      vec3 up;
      float _p2; // 32
      vec3 forward;
      float _p3;        // 48
      float tanHalfFov; // 64
      float aspect;     // 68
      int moving;       // 72
      int _pad4;        // 76
      int resX;         // 80
      int resY;         // 84
    } d;
    vec3 fwd = normalize(cam.target - cam.position());
    vec3 up = vec3(0, 1, 0);
    vec3 right = normalize(cross(fwd, up));
    up = cross(right, fwd);
    d.pos = cam.position();
    d.right = right;
    d.up = up;
    d.forward = fwd;
    d.tanHalfFov = tan(radians(30.0f));
    d.aspect = float(WIDTH) / float(HEIGHT); // [FIX] window aspect
    d.moving = cam.moving ? 1 : 0;
    d.resX = cam.moving ? COMPUTE_WIDTH_LOW : COMPUTE_WIDTH;   // [FIX]
    d.resY = cam.moving ? COMPUTE_HEIGHT_LOW : COMPUTE_HEIGHT; // [FIX]
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
  }

  // [FIX] mass promoted to vec4[16] (matches std140 layout in shader)
  void uploadObjectsUBO(const vector<ObjectData> &objs) {
    struct UBO {
      int n;
      float _p[3];
      vec4 posR[16];
      vec4 col[16];
      vec4 mass[16]; // [FIX] was float[16]
    } d;
    size_t cnt = (objs.size() < 16) ? objs.size() : 16;
    d.n = int(cnt);
    for (size_t i = 0; i < cnt; ++i) {
      d.posR[i] = objs[i].posRadius;
      d.col[i] = objs[i].color;
      d.mass[i] = vec4(float(objs[i].mass), 0.0f, 0.0f, 0.0f); // [FIX]
    }
    glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
  }

  // [FIX] 3rd slot now carries SagA.r_s (was disk_num, unused in shader)
  void uploadDiskUBO(float t) {
    float r1 = float(SagA.r_s * 3.0);
    float r2 = float(SagA.r_s * 6.0);
    float d[4] = {r1, r2, float(SagA.r_s), t}; // [ANIM] 4th slot = animation clock // [FIX] 3rd = bhRs
    glBindBuffer(GL_UNIFORM_BUFFER, diskUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, 16, d);
  }

  vector<GLuint> QuadVAO() {
    float v[] = {-1, 1, 0, 1, -1, -1, 0, 0, 1, -1, 1, 0,
                 -1, 1, 0, 1, 1,  -1, 1, 0, 1, 1,  1, 1};
    GLuint VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, COMPUTE_WIDTH, COMPUTE_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return {VAO, tex};
  }

  // [FIX] particle shader build moved here from generateParticles()
  void createParticleShader() {
    if (particleProgram)
      return;
    const char *vs = R"(
            #version 330 core
            layout(location=0) in vec3 aPos;
            layout(location=1) in vec3 aVel;
            layout(location=2) in float aLife;
            uniform mat4 viewProj;
            uniform float uMaxAge;
            out float life;
            void main() {
                life = clamp(aLife / uMaxAge, 0.0, 1.0);
                gl_Position = viewProj * vec4(aPos, 1.0);
                gl_PointSize = 3.0 * (1.0 - life) + 1.0;
            }
        )";
    const char *fs = R"(
            #version 330 core
            in float life;
            out vec4 FragColor;
            void main() {
                vec3 c = mix(vec3(1.0, 0.3, 0.0), vec3(1.0, 1.0, 0.5), life);
                float a = (1.0 - life) * 0.8;
                FragColor = vec4(c, a);
            }
        )";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);
    particleProgram = glCreateProgram();
    glAttachShader(particleProgram, v);
    glAttachShader(particleProgram, f);
    glLinkProgram(particleProgram);
    glDeleteShader(v);
    glDeleteShader(f);
  }

  // [NEW] animated photon stream: pool of photons falling in from all sides,
  // respawned on capture/escape so the inflow never stops
  struct Photon {
    vec3 pos;
    vec3 vel;
    float age;
  };
  vector<Photon> photons;
  const float PHOTON_SPAWN_R = 6e10f;
  const float PHOTON_KILL_R = 1.5e11f;
  const float PHOTON_MAX_AGE = 20.0f;
  const float PHOTON_SPEED = 2e8f; // ~0.67 c

  void spawnPhoton(Photon &p) {
    const float rs = float(SagA.r_s);
    // uniform direction on a sphere — arrives from a random side
    float u = (float(rand()) / RAND_MAX) * 2.0f - 1.0f;
    float a = (float(rand()) / RAND_MAX) * 2.0f * float(M_PI);
    float s = sqrt(1.0f - u * u);
    vec3 n = vec3(s * cos(a), u, s * sin(a));
    p.pos = n * PHOTON_SPAWN_R;

    // random impact parameter: paths capture, slingshot, or graze
    vec3 tangent = normalize(cross(n, vec3(0.0f, 1.0f, 0.3f)));
    vec3 bitan = normalize(cross(n, tangent));
    float b = ((float(rand()) / RAND_MAX) * 2.0f - 1.0f) * 2.5f * rs;
    float c2 = ((float(rand()) / RAND_MAX) * 2.0f - 1.0f) * 1.5f * rs;
    p.vel = normalize(tangent * b + bitan * c2 - p.pos) * PHOTON_SPEED;
    p.age = 0.0f;
  }

  void generateParticles() {
    photons.resize(1500);
    for (auto &p : photons)
      spawnPhoton(p);

    particleCount = (int)photons.size();
    if (!particleVAO)
      glGenVertexArrays(1, &particleVAO);
    if (!particleVBO)
      glGenBuffers(1, &particleVBO);
    glBindVertexArray(particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, particleVBO);
    glBufferData(GL_ARRAY_BUFFER, photons.size() * sizeof(Photon), nullptr,
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Photon), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Photon),
                          (void *)offsetof(Photon, vel));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Photon),
                          (void *)offsetof(Photon, age));
    glBindVertexArray(0);
  }

  // integrate one frame; Photon.age doubles as the shader's `life` attribute
  void updatePhotons(float dtFrame) {
    if (photons.empty())
      return;
    const float rs = float(SagA.r_s);
    float dt = dtFrame * 20.0f; // sim-speed multiplier
    for (auto &p : photons) {
      float r = length(p.pos);
      vec3 force = -vec3(float(G) * SagA.mass / (r * r * r)) * p.pos;
      p.vel += force * dt;
      p.pos += p.vel * dt;
      p.age += dt;
      // respawn consumed photons -> continuous stream
      if (r < rs || r > PHOTON_KILL_R || p.age > PHOTON_MAX_AGE)
        spawnPhoton(p);
    }
    glBindVertexArray(particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, particleVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, photons.size() * sizeof(Photon),
                    photons.data());
    glBindVertexArray(0);
  }

  void drawParticles(const mat4 &vp) {
    if (!ShowParticles || particleCount == 0)
      return;
    glUseProgram(particleProgram);
    glUniformMatrix4fv(glGetUniformLocation(particleProgram, "viewProj"), 1,
                       GL_FALSE, value_ptr(vp));
    glUniform1f(glGetUniformLocation(particleProgram, "uMaxAge"),
                PHOTON_MAX_AGE);
    glBindVertexArray(particleVAO);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_POINTS, 0, particleCount);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
  }
};
Engine engine;

void setupCallbacks(GLFWwindow *w) {
  glfwSetWindowUserPointer(w, &camera);
  glfwSetMouseButtonCallback(w, [](GLFWwindow *win, int b, int a, int m) {
    Camera *c = (Camera *)glfwGetWindowUserPointer(win);
    c->processMouseButton(b, a, m, win);
  });
  glfwSetCursorPosCallback(w, [](GLFWwindow *win, double x, double y) {
    Camera *c = (Camera *)glfwGetWindowUserPointer(win);
    c->processMouseMove(x, y);
  });
  glfwSetScrollCallback(w, [](GLFWwindow *win, double xo, double yo) {
    Camera *c = (Camera *)glfwGetWindowUserPointer(win);
    c->processScroll(xo, yo);
  });
  glfwSetKeyCallback(w, [](GLFWwindow *win, int k, int sc, int a, int m) {
    Camera *c = (Camera *)glfwGetWindowUserPointer(win);
    c->processKey(k, sc, a, m);
  });
  // [FIX] framebuffer resize callback
  glfwSetFramebufferSizeCallback(
      w, [](GLFWwindow *win, int w, int h) { engine.onResize(w, h); });
}

int main() {
  setupCallbacks(engine.window);
  double lastTime = glfwGetTime();
  bool gridDirty = true;
  double lastGridTime = glfwGetTime();

  while (!glfwWindowShouldClose(engine.window)) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    double now = glfwGetTime();
    double dt = now - lastTime;
    lastTime = now;
    if (dt > 0.05)
      dt = 0.05; // clamp hitches so orbits stay stable

    // [ANIM] cinematic auto-orbit when idle (rides the low-res path)
    if (AutoOrbit && !camera.dragging) {
      camera.azimuth += 0.05f * float(dt);
      camera.moving = true;
      ViewDirty = true;
    }

    // [FIX] either sticky toggle OR momentary hold triggers gravity
    if (Gravity || GravityPull) {
      const float sdt = float(dt) * 800.0f; // [ANIM] time acceleration: visible orbits
      for (size_t i = 0; i < objects.size(); ++i) {
        auto &o = objects[i];
        float ax = 0, ay = 0, az = 0;
        for (size_t j = 0; j < objects.size(); ++j)
          if (i != j) {
            auto &o2 = objects[j];
            float dx = o2.posRadius.x - o.posRadius.x,
                  dy = o2.posRadius.y - o.posRadius.y,
                  dz = o2.posRadius.z - o.posRadius.z;
            float d2 = dx * dx + dy * dy + dz * dz, d = sqrt(d2);
            if (d > 0) {
              float f = float(G) * o2.mass / (d2 * d);
              ax += dx * f;
              ay += dy * f;
              az += dz * f;
            }
          }
        o.velocity.x += ax * sdt;
        o.velocity.y += ay * sdt;
        o.velocity.z += az * sdt;
        o.posRadius.x += o.velocity.x * sdt;
        o.posRadius.y += o.velocity.y * sdt;
        o.posRadius.z += o.velocity.z * sdt;
      }
      gridDirty = true;
    }
    // [FIX] throttle grid rebuilds to 50 ms while gravity is active
    if (gridDirty && (now - lastGridTime) > 0.05) {
      engine.generateGrid(objects);
      gridDirty = false;
      lastGridTime = now;
    }

    glViewport(0, 0, engine.WIDTH, engine.HEIGHT);
    // [PERF] raytrace only when view/scene changed; otherwise re-present the
    // cached texture (idle frames cost ~nothing)
    if (ViewDirty || Gravity || GravityPull) {
      engine.dispatchCompute(camera);
      ViewDirty = false;
    }
    engine.drawFullScreenQuad();

    mat4 view = lookAt(camera.position(), camera.target, vec3(0, 1, 0));
    mat4 proj =
        perspective(radians(60.0f), float(engine.WIDTH) / float(engine.HEIGHT),
                    1e9f, 1e14f);
    engine.drawGrid(proj * view);
    if (ShowParticles)
      engine.updatePhotons(float(dt));
    engine.drawParticles(proj * view);

    glfwSwapBuffers(engine.window);
    glfwPollEvents();
  }
  glfwDestroyWindow(engine.window);
  glfwTerminate();
  return 0;
}