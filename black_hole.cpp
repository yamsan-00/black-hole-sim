#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <iostream>
#define _USE_MATH_DEFINES
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <fstream>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace glm;
using namespace std;
using Clock = std::chrono::high_resolution_clock;

double c = 299792458.0;
double G = 6.67430e-11;
bool Gravity = false;

struct Camera {
    vec3 target = vec3(0.0f);
    float radius = 6.34194e10f;
    float minRadius = 1e10f, maxRadius = 1e12f;
    float azimuth = 0.0f;
    float elevation = M_PI / 2.0f;
    float orbitSpeed = 0.01f;
    double zoomSpeed = 25e9f;
    bool dragging = false;
    bool moving = false;
    double lastX = 0.0, lastY = 0.0;

    vec3 position() const {
        float clampedElevation = glm::clamp(elevation, 0.01f, float(M_PI) - 0.01f);
        return vec3(
            radius * sin(clampedElevation) * cos(azimuth),
            radius * cos(clampedElevation),
            radius * sin(clampedElevation) * sin(azimuth)
        );
    }
    void update() {
        target = vec3(0.0f);
        moving = dragging;
    }
    void processMouseMove(double x, double y) {
        float dx = float(x - lastX);
        float dy = float(y - lastY);
        if (dragging) {
            azimuth   += dx * orbitSpeed;
            elevation -= dy * orbitSpeed;
            elevation = glm::clamp(elevation, 0.01f, float(M_PI) - 0.01f);
        }
        lastX = x; lastY = y;
        update();
    }
    void processMouseButton(int button, int action, int mods, GLFWwindow* win) {
        if (button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == GLFW_PRESS) {
                dragging = true;
                glfwGetCursorPos(win, &lastX, &lastY);
            } else if (action == GLFW_RELEASE) {
                dragging = false;
            }
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) Gravity = true;
            else if (action == GLFW_RELEASE) Gravity = false;
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
            cout << "[INFO] Gravity " << (Gravity ? "ON" : "OFF") << endl;
        }
    }
};
Camera camera;

struct BlackHole {
    vec3 position;
    double mass;
    double r_s;
    BlackHole(vec3 pos, float m) : position(pos), mass(m) { r_s = 2.0 * G * mass / (c*c); }
};
BlackHole SagA(vec3(0.0f), 8.54e36);

struct ObjectData {
    vec4 posRadius;
    vec4 color;
    float mass;
    vec3 velocity = vec3(0.0f);
};
vector<ObjectData> objects = {
    { vec4(4e11f, 0.0f, 0.0f, 4e10f), vec4(1,1,0,1), 1.98892e30 },
    { vec4(0.0f, 0.0f, 4e11f, 4e10f), vec4(1,0,0,1), 1.98892e30 },
    { vec4(0.0f, 0.0f, 0.0f, float(SagA.r_s)), vec4(0,0,0,1), float(SagA.mass) },
};

struct Engine {
    GLuint gridShaderProgram = 0;
    GLFWwindow* window = nullptr;
    GLuint quadVAO = 0, texture = 0, shaderProgram = 0, computeProgram = 0;
    GLuint cameraUBO = 0, diskUBO = 0, objectsUBO = 0;
    GLuint gridVAO = 0, gridVBO = 0, gridEBO = 0;
    int gridIndexCount = 0;

    int WIDTH = 800, HEIGHT = 600;
    int COMPUTE_WIDTH = 200, COMPUTE_HEIGHT = 150;
    int COMPUTE_WIDTH_LOW = 128, COMPUTE_HEIGHT_LOW = 96;

    Engine() {
        if (!glfwInit()) { cerr << "GLFW init failed\n"; exit(EXIT_FAILURE); }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(WIDTH, HEIGHT, "Black Hole", nullptr, nullptr);
        if (!window) { cerr << "Failed to create GLFW window\n"; glfwTerminate(); exit(EXIT_FAILURE); }
        glfwMakeContextCurrent(window);
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) { cerr << "GLEW init failed\n"; glfwTerminate(); exit(EXIT_FAILURE); }
        cout << "OpenGL " << glGetString(GL_VERSION) << "\n";

        shaderProgram = CreateShaderProgram();
        gridShaderProgram = CreateShaderProgram("grid.vert", "grid.frag");
        computeProgram = CreateComputeProgram("geodesic.comp");

        glGenBuffers(1, &cameraUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
        glBufferData(GL_UNIFORM_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, cameraUBO);

        glGenBuffers(1, &diskUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, diskUBO);
        glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, diskUBO);

        glGenBuffers(1, &objectsUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
        GLsizeiptr sz = sizeof(int) + 3*sizeof(float) + 16*(sizeof(vec4)+sizeof(vec4)) + 16*sizeof(float);
        glBufferData(GL_UNIFORM_BUFFER, sz, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, objectsUBO);

        auto r = QuadVAO();
        quadVAO = r[0]; texture = r[1];

        generateGrid(objects);
    }

    void generateGrid(const vector<ObjectData>& objs) {
        const int gridSize = 25;
        const float spacing = 1e10f;
        vector<vec3> verts; verts.reserve((gridSize+1)*(gridSize+1));
        vector<GLuint> idx; idx.reserve(gridSize*gridSize*4);

        for (int z=0; z<=gridSize; ++z) {
            for (int x=0; x<=gridSize; ++x) {
                float wx = (x - gridSize/2) * spacing;
                float wz = (z - gridSize/2) * spacing;
                float y = 0.0f;
                for (auto& o : objs) {
                    vec3 op = vec3(o.posRadius);
                    double rs = 2.0 * G * o.mass / (c*c);
                    double dx = wx - op.x, dz = wz - op.z;
                    double d = sqrt(dx*dx + dz*dz);
                    if (d > rs) y += float(2.0 * sqrt(rs * (d - rs))) - 3e10f;
                    else y += 2.0f * float(sqrt(rs*rs)) - 3e10f;
                }
                verts.emplace_back(wx, y, wz);
            }
        }
        for (int z=0; z<gridSize; ++z) {
            for (int x=0; x<gridSize; ++x) {
                int i = z*(gridSize+1)+x;
                idx.push_back(i); idx.push_back(i+1);
                idx.push_back(i); idx.push_back(i+gridSize+1);
            }
        }

        if (!gridVAO) glGenVertexArrays(1, &gridVAO);
        if (!gridVBO) glGenBuffers(1, &gridVBO);
        if (!gridEBO) glGenBuffers(1, &gridEBO);

        glBindVertexArray(gridVAO);
        glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(vec3), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gridEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(GLuint), idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3), (void*)0);
        gridIndexCount = (int)idx.size();
        glBindVertexArray(0);
    }

    void drawGrid(const mat4& vp) {
        glUseProgram(gridShaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(gridShaderProgram, "viewProj"), 1, GL_FALSE, value_ptr(vp));
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

    GLuint CreateShaderProgram() {
        const char* vs = R"(
            #version 330 core
            layout(location=0) in vec2 aPos;
            layout(location=1) in vec2 aTexCoord;
            out vec2 TexCoord;
            void main() { gl_Position = vec4(aPos, 0.0, 1.0); TexCoord = aTexCoord; }
        )";
        const char* fs = R"(
            #version 330 core
            in vec2 TexCoord; out vec4 FragColor;
            uniform sampler2D screenTexture;
            void main() { FragColor = texture(screenTexture, TexCoord); }
        )";
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vs, nullptr); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fs, nullptr); glCompileShader(f);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }
    GLuint CreateShaderProgram(const char* vp, const char* fp) {
        auto load = [](const char* path, GLenum t) {
            ifstream in(path); if (!in.is_open()) { cerr << "Shader missing: " << path << "\n"; exit(1); }
            stringstream ss; ss << in.rdbuf(); string s = ss.str(); const char* c = s.c_str();
            GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &c, nullptr); glCompileShader(sh);
            GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) { GLint len; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
                vector<char> log(len); glGetShaderInfoLog(sh, len, nullptr, log.data());
                cerr << "Shader error " << path << ": " << log.data() << "\n"; exit(1); }
            return sh;
        };
        GLuint v = load(vp, GL_VERTEX_SHADER);
        GLuint f = load(fp, GL_FRAGMENT_SHADER);
        GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { GLint len; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
            vector<char> log(len); glGetProgramInfoLog(p, len, nullptr, log.data());
            cerr << "Link error: " << log.data() << "\n"; exit(1); }
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }
    GLuint CreateComputeProgram(const char* path) {
        ifstream in(path); if (!in.is_open()) { cerr << "Compute missing: " << path << "\n"; exit(1); }
        stringstream ss; ss << in.rdbuf(); string s = ss.str(); const char* c = s.c_str();
        GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(cs, 1, &c, nullptr); glCompileShader(cs);
        GLint ok; glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
        if (!ok) { GLint len; glGetShaderiv(cs, GL_INFO_LOG_LENGTH, &len);
            vector<char> log(len); glGetShaderInfoLog(cs, len, nullptr, log.data());
            cerr << "Compute error: " << log.data() << "\n"; exit(1); }
        GLuint p = glCreateProgram(); glAttachShader(p, cs); glLinkProgram(p);
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { GLint len; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
            vector<char> log(len); glGetProgramInfoLog(p, len, nullptr, log.data());
            cerr << "Compute link: " << log.data() << "\n"; exit(1); }
        glDeleteShader(cs); return p;
    }

    void dispatchCompute(const Camera& cam) {
        bool mov = cam.moving;
        int cw = mov ? COMPUTE_WIDTH_LOW : COMPUTE_WIDTH;
        int ch = mov ? COMPUTE_HEIGHT_LOW : COMPUTE_HEIGHT;

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glUseProgram(computeProgram);
        uploadCameraUBO(cam);
        uploadDiskUBO();
        uploadObjectsUBO(objects);

        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        GLuint gx = (cw + 15) / 16;
        GLuint gy = (ch + 15) / 16;
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    void uploadCameraUBO(const Camera& cam) {
        struct UBO { vec3 pos, right, up, forward; float tanHalfFov, aspect; int moving; float _pad[2]; } d;
        vec3 fwd = normalize(cam.target - cam.position());
        vec3 up = vec3(0,1,0);
        vec3 right = normalize(cross(fwd, up));
        up = cross(right, fwd);
        d.pos = cam.position(); d.right = right; d.up = up; d.forward = fwd;
        d.tanHalfFov = tan(radians(30.0f)); d.aspect = float(WIDTH)/float(HEIGHT);
        d.moving = cam.moving ? 1 : 0;
        glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }

    void uploadObjectsUBO(const vector<ObjectData>& objs) {
        struct UBO { int n; float _p[3]; vec4 posR[16], col[16]; float mass[16]; } d;
        size_t cnt = (objs.size() < 16) ? objs.size() : 16;
        d.n = int(cnt);
        for (size_t i=0; i<cnt; ++i) {
            d.posR[i] = objs[i].posRadius;
            d.col[i] = objs[i].color;
            d.mass[i] = objs[i].mass;
        }
        glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }

    void uploadDiskUBO() {
        float r1 = float(SagA.r_s * 2.2), r2 = float(SagA.r_s * 5.2);
        float d[4] = { r1, r2, 2.0f, 1e9f };
        glBindBuffer(GL_UNIFORM_BUFFER, diskUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, 16, d);
    }

    vector<GLuint> QuadVAO() {
        float v[] = { -1,1,0,1, -1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,-1,1,0, 1,1,1,1 };
        GLuint VAO, VBO; glGenVertexArrays(1, &VAO); glGenBuffers(1, &VBO);
        glBindVertexArray(VAO); glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); glEnableVertexAttribArray(1);
        GLuint tex; glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, COMPUTE_WIDTH, COMPUTE_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        return {VAO, tex};
    }
};
Engine engine;

void setupCallbacks(GLFWwindow* w) {
    glfwSetWindowUserPointer(w, &camera);
    glfwSetMouseButtonCallback(w, [](GLFWwindow* win, int b, int a, int m) {
        Camera* c = (Camera*)glfwGetWindowUserPointer(win); c->processMouseButton(b,a,m,win);
    });
    glfwSetCursorPosCallback(w, [](GLFWwindow* win, double x, double y) {
        Camera* c = (Camera*)glfwGetWindowUserPointer(win); c->processMouseMove(x,y);
    });
    glfwSetScrollCallback(w, [](GLFWwindow* win, double xo, double yo) {
        Camera* c = (Camera*)glfwGetWindowUserPointer(win); c->processScroll(xo,yo);
    });
    glfwSetKeyCallback(w, [](GLFWwindow* win, int k, int sc, int a, int m) {
        Camera* c = (Camera*)glfwGetWindowUserPointer(win); c->processKey(k,sc,a,m);
    });
}

int main() {
    setupCallbacks(engine.window);
    double lastTime = glfwGetTime();
    bool gridDirty = true;

    while (!glfwWindowShouldClose(engine.window)) {
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        double now = glfwGetTime(); double dt = now - lastTime; lastTime = now;

        if (Gravity) {
            for (size_t i=0; i<objects.size(); ++i) {
                auto& o = objects[i]; float ax=0,ay=0,az=0;
                for (size_t j=0; j<objects.size(); ++j) if (i!=j) {
                    auto& o2 = objects[j];
                    float dx=o2.posRadius.x-o.posRadius.x, dy=o2.posRadius.y-o.posRadius.y, dz=o2.posRadius.z-o.posRadius.z;
                    float d2=dx*dx+dy*dy+dz*dz, d=sqrt(d2);
                    if (d>0) { float f = float(G)*o2.mass/(d2*d); ax+=dx*f; ay+=dy*f; az+=dz*f; }
                }
                o.velocity.x += ax*float(dt); o.velocity.y += ay*float(dt); o.velocity.z += az*float(dt);
                o.posRadius.x += o.velocity.x*float(dt); o.posRadius.y += o.velocity.y*float(dt); o.posRadius.z += o.velocity.z*float(dt);
            }
            gridDirty = true;
        }
        if (gridDirty) { engine.generateGrid(objects); gridDirty = false; }

        mat4 view = lookAt(camera.position(), camera.target, vec3(0,1,0));
        mat4 proj = perspective(radians(60.0f), float(engine.WIDTH)/float(engine.HEIGHT), 1e9f, 1e14f);
        engine.drawGrid(proj * view);

        glViewport(0, 0, engine.WIDTH, engine.HEIGHT);
        engine.dispatchCompute(camera);
        engine.drawFullScreenQuad();

        glfwSwapBuffers(engine.window);
        glfwPollEvents();
    }
    glfwDestroyWindow(engine.window); glfwTerminate(); return 0;
}