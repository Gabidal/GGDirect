#include "renderer.h"
#include "types.h"
#include "display.h"
#include "font.h"
#include "logger.h"
#include "config.h"
#include "window.h"
#include "gpu_context.h"

#include <thread>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <GLES2/gl2.h>

namespace renderer {

namespace {

struct ShaderProgram {
    GLuint program = 0;
    GLint attrPosition = -1;
    GLint attrTexCoord = -1;
    GLint uniformProjection = -1;
    GLint uniformTexture = -1;
};

struct HandleGpuResources {
    GLuint textureId = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<uint32_t> pixelBuffer;
};

struct OptimizedCellCache {
    types::Cell cellID{};
    std::vector<uint32_t> rgbaPixels;
    int width = 0;
    int height = 0;
    bool isValid = false;

    void initialize(int w, int h) {
        width = w;
        height = h;
        rgbaPixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        isValid = false;
    }

    void convertFromRGB(const font::cellRenderData& cellData) {
        if (rgbaPixels.size() != static_cast<size_t>(cellData.width * cellData.height)) {
            rgbaPixels.resize(static_cast<size_t>(cellData.width) * static_cast<size_t>(cellData.height));
        }

        const size_t pixelCount = cellData.pixels.size();
        for (size_t i = 0; i < pixelCount; ++i) {
            const types::RGB& rgb = cellData.pixels[i];
            rgbaPixels[i] = static_cast<uint32_t>(rgb.r) |
                            (static_cast<uint32_t>(rgb.g) << 8) |
                            (static_cast<uint32_t>(rgb.b) << 16) |
                            0xFF000000u;
        }

        width = cellData.width;
        height = cellData.height;
        isValid = true;
    }
};

ShaderProgram shaderProgram;
GLuint quadVbo = 0;
float projectionMatrix[16] = {0.0f};

GLuint wallpaperTexture = 0;
int wallpaperWidth = 0;
int wallpaperHeight = 0;
std::string wallpaperPath;
bool wallpaperReady = false;

std::shared_ptr<display::connector> primaryConnector;
std::unique_ptr<display::mode> currentMode;

bool rendererInitialized = false;
bool shouldExit = false;

std::unordered_map<const window::handle*, HandleGpuResources> handleResources;
types::iVector2 framebufferResolution;

gpu::Context gpuContext;

constexpr GLsizei kVerticesPerQuad = 4;
constexpr GLsizei kFloatsPerVertex = 4;
constexpr GLsizei kVertexBufferSize = kVerticesPerQuad * kFloatsPerVertex;

uint32_t makeRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void updateProjectionMatrix(int width, int height) {
    std::fill(std::begin(projectionMatrix), std::end(projectionMatrix), 0.0f);
    projectionMatrix[0] = 2.0f / static_cast<float>(width);
    projectionMatrix[5] = -2.0f / static_cast<float>(height);
    projectionMatrix[10] = -1.0f;
    projectionMatrix[12] = -1.0f;
    projectionMatrix[13] = 1.0f;
    projectionMatrix[15] = 1.0f;
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        return 0;
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(logLength));
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        LOG_ERROR() << "Shader compilation failed: " << log.data() << std::endl;
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool createShaderProgram() {
    static const char* vertexSrc = R"(
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
        uniform mat4 u_projection;
        varying vec2 v_texCoord;
        void main() {
            gl_Position = u_projection * vec4(a_position, 0.0, 1.0);
            v_texCoord = a_texCoord;
        }
    )";

    static const char* fragmentSrc = R"(
        precision mediump float;
        varying vec2 v_texCoord;
        uniform sampler2D u_texture;
        void main() {
            gl_FragColor = texture2D(u_texture, v_texCoord);
        }
    )";

    GLuint vert = compileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    shaderProgram.program = glCreateProgram();
    if (!shaderProgram.program) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    glAttachShader(shaderProgram.program, vert);
    glAttachShader(shaderProgram.program, frag);
    glBindAttribLocation(shaderProgram.program, 0, "a_position");
    glBindAttribLocation(shaderProgram.program, 1, "a_texCoord");
    glLinkProgram(shaderProgram.program);

    GLint status = GL_FALSE;
    glGetProgramiv(shaderProgram.program, GL_LINK_STATUS, &status);
    glDeleteShader(vert);
    glDeleteShader(frag);

    if (status != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(shaderProgram.program, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(logLength));
        glGetProgramInfoLog(shaderProgram.program, logLength, nullptr, log.data());
        LOG_ERROR() << "Program linking failed: " << log.data() << std::endl;
        glDeleteProgram(shaderProgram.program);
        shaderProgram.program = 0;
        return false;
    }

    shaderProgram.attrPosition = glGetAttribLocation(shaderProgram.program, "a_position");
    shaderProgram.attrTexCoord = glGetAttribLocation(shaderProgram.program, "a_texCoord");
    shaderProgram.uniformProjection = glGetUniformLocation(shaderProgram.program, "u_projection");
    shaderProgram.uniformTexture = glGetUniformLocation(shaderProgram.program, "u_texture");

    return shaderProgram.attrPosition >= 0 && shaderProgram.attrTexCoord >= 0 &&
           shaderProgram.uniformProjection >= 0 && shaderProgram.uniformTexture >= 0;
}

void destroyShaderProgram() {
    if (shaderProgram.program) {
        glDeleteProgram(shaderProgram.program);
        shaderProgram = {};
    }
}

void ensureQuadBuffer() {
    if (!quadVbo) {
        glGenBuffers(1, &quadVbo);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * kVertexBufferSize, nullptr, GL_DYNAMIC_DRAW);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
    }
}

void destroyQuadBuffer() {
    if (quadVbo) {
        glDeleteBuffers(1, &quadVbo);
        quadVbo = 0;
    }
}

void destroyWallpaperTexture() {
    if (wallpaperTexture) {
        glDeleteTextures(1, &wallpaperTexture);
        wallpaperTexture = 0;
        wallpaperReady = false;
        wallpaperWidth = 0;
        wallpaperHeight = 0;
    }
}

void uploadWallpaperTextureIfNeeded() {
    std::string desiredPath = config::manager::getWallpaperPath();
    if (desiredPath.empty()) {
        destroyWallpaperTexture();
        wallpaperPath.clear();
        return;
    }

    if (wallpaperReady && desiredPath == wallpaperPath) {
        return;
    }

    const uint32_t* data = nullptr;
    int width = 0;
    int height = 0;
    if (!config::manager::getWallpaperData(data, width, height) || !data) {
        destroyWallpaperTexture();
        wallpaperPath.clear();
        return;
    }

    std::vector<uint32_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t i = 0; i < rgba.size(); ++i) {
        uint32_t pixel = data[i];
        uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xFF);
        uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xFF);
        uint8_t b = static_cast<uint8_t>(pixel & 0xFF);
        rgba[i] = makeRGBA(r, g, b, 0xFF);
    }

    if (!wallpaperTexture) {
        glGenTextures(1, &wallpaperTexture);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wallpaperTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    wallpaperWidth = width;
    wallpaperHeight = height;
    wallpaperReady = true;
    wallpaperPath = desiredPath;
}

void destroyHandleResources(HandleGpuResources& resources) {
    if (resources.textureId) {
        glDeleteTextures(1, &resources.textureId);
        resources.textureId = 0;
    }
    resources.pixelBuffer.clear();
    resources.pixelWidth = 0;
    resources.pixelHeight = 0;
}

void drawQuad(GLuint texture, float x, float y, float width, float height) {
    float vertices[kVertexBufferSize] = {
        x,          y,          0.0f, 0.0f,
        x + width,  y,          1.0f, 0.0f,
        x,          y + height, 0.0f, 1.0f,
        x + width,  y + height, 1.0f, 1.0f
    };

    ensureQuadBuffer();
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    glUseProgram(shaderProgram.program);
    glUniformMatrix4fv(shaderProgram.uniformProjection, 1, GL_FALSE, projectionMatrix);
    glUniform1i(shaderProgram.uniformTexture, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glEnableVertexAttribArray(shaderProgram.attrPosition);
    glEnableVertexAttribArray(shaderProgram.attrTexCoord);
    glVertexAttribPointer(shaderProgram.attrPosition, 2, GL_FLOAT, GL_FALSE, sizeof(float) * kFloatsPerVertex, reinterpret_cast<void*>(0));
    glVertexAttribPointer(shaderProgram.attrTexCoord, 2, GL_FLOAT, GL_FALSE, sizeof(float) * kFloatsPerVertex, reinterpret_cast<void*>(sizeof(float) * 2));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void blitCellToBuffer(uint32_t* buffer, int bufferWidth, int bufferHeight, int startX, int startY, const OptimizedCellCache& cache) {
    if (!buffer || !cache.isValid) {
        return;
    }

    if (startX >= bufferWidth || startY >= bufferHeight) {
        return;
    }

    int copyWidth = std::min(cache.width, bufferWidth - startX);
    int copyHeight = std::min(cache.height, bufferHeight - startY);

    for (int y = 0; y < copyHeight; ++y) {
        size_t srcOffset = static_cast<size_t>(y) * static_cast<size_t>(cache.width);
        size_t dstOffset = static_cast<size_t>(startY + y) * static_cast<size_t>(bufferWidth) + static_cast<size_t>(startX);
        std::memcpy(&buffer[dstOffset], &cache.rgbaPixels[srcOffset], static_cast<size_t>(copyWidth) * sizeof(uint32_t));
    }
}

HandleGpuResources& getHandleResources(const window::handle* handle, int pixelWidth, int pixelHeight) {
    auto& resources = handleResources[handle];
    if (resources.pixelWidth != pixelWidth || resources.pixelHeight != pixelHeight) {
        resources.pixelWidth = pixelWidth;
        resources.pixelHeight = pixelHeight;
        resources.pixelBuffer.resize(static_cast<size_t>(pixelWidth) * static_cast<size_t>(pixelHeight));

        if (resources.textureId) {
            glDeleteTextures(1, &resources.textureId);
            resources.textureId = 0;
        }
    }
    return resources;
}

void removeStaleHandleResources(const std::unordered_set<const window::handle*>& activeHandles) {
    for (auto it = handleResources.begin(); it != handleResources.end();) {
        if (activeHandles.find(it->first) == activeHandles.end()) {
            destroyHandleResources(it->second);
            it = handleResources.erase(it);
        } else {
            ++it;
        }
    }
}

bool uploadHandleTexture(HandleGpuResources& resources) {
    if (resources.pixelBuffer.empty() || resources.pixelWidth <= 0 || resources.pixelHeight <= 0) {
        return false;
    }

    if (!resources.textureId) {
        glGenTextures(1, &resources.textureId);
        glBindTexture(GL_TEXTURE_2D, resources.textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, resources.pixelWidth, resources.pixelHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, resources.pixelBuffer.data());
    } else {
        glBindTexture(GL_TEXTURE_2D, resources.textureId);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, resources.pixelWidth, resources.pixelHeight, GL_RGBA, GL_UNSIGNED_BYTE, resources.pixelBuffer.data());
    }

    return true;
}

bool renderHandleInternal(const window::handle* handle) {
    if (!handle || handle->connection.isClosed()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(handle->cellBufferMutex);
    if (!handle->cellBuffer || handle->cellBuffer->empty()) {
        return false;
    }

    types::rectangle windowCellRect = handle->getCellCoordinates();
    types::rectangle windowPixelRect = handle->getPixelCoordinates();

    if (windowCellRect.size.x <= 0 || windowCellRect.size.y <= 0) {
        return false;
    }

    auto* fontPtr = handle->getFont();
    int baseCellWidth = font::manager::getDefaultCellWidth();
    int baseCellHeight = font::manager::getDefaultCellHeight();
    int cellWidth = static_cast<int>(baseCellWidth * handle->zoom);
    int cellHeight = static_cast<int>(baseCellHeight * handle->zoom);

    int windowWidth = windowCellRect.size.x * cellWidth;
    int windowHeight = windowCellRect.size.y * cellHeight;

    auto& resources = getHandleResources(handle, windowWidth, windowHeight);

    OptimizedCellCache cellCache;
    cellCache.initialize(cellWidth, cellHeight);

    font::cellRenderData tempBuffer{cellWidth, cellHeight, {}};
    tempBuffer.pixels.resize(static_cast<size_t>(cellWidth) * static_cast<size_t>(cellHeight));

    for (int cellY = 0; cellY < windowCellRect.size.y; ++cellY) {
        for (int cellX = 0; cellX < windowCellRect.size.x; ++cellX) {
            int cellIndex = cellY * windowCellRect.size.x + cellX;
            if (cellIndex < 0 || static_cast<size_t>(cellIndex) >= handle->cellBuffer->size()) {
                continue;
            }

            const types::Cell& cell = (*handle->cellBuffer)[cellIndex];
            if (!cellCache.isValid || cellCache.cellID != cell) {
                std::fill(tempBuffer.pixels.begin(), tempBuffer.pixels.end(), cell.backgroundColor);
                if (fontPtr) {
                    tempBuffer = fontPtr->renderCell(cell, tempBuffer, handle->zoom);
                }
                cellCache.convertFromRGB(tempBuffer);
                cellCache.cellID = cell;
            }

            int bufferStartX = cellX * cellWidth;
        int bufferStartY = cellY * cellHeight;
        blitCellToBuffer(resources.pixelBuffer.data(), windowWidth, windowHeight, bufferStartX, bufferStartY, cellCache);
        }
    }

    if (!uploadHandleTexture(resources)) {
        return false;
    }

    drawQuad(resources.textureId,
             static_cast<float>(windowPixelRect.position.x),
             static_cast<float>(windowPixelRect.position.y),
             static_cast<float>(windowWidth),
             static_cast<float>(windowHeight));

    return true;
}

void renderWallpaper() {
    if (!wallpaperReady || !wallpaperTexture) {
        return;
    }

    drawQuad(wallpaperTexture, 0.0f, 0.0f, static_cast<float>(framebufferResolution.x), static_cast<float>(framebufferResolution.y));
}

void releaseAllHandleResources() {
    for (auto& entry : handleResources) {
        destroyHandleResources(entry.second);
    }
    handleResources.clear();
}

void registerPageFlipHook() {
    display::manager::setPageFlipCompletionHook([]() {
        if (display::manager::Device) {
            gpuContext.onPageFlipComplete(*display::manager::Device);
        }
    });
}

} // namespace

void init() {
    if (rendererInitialized) {
        return;
    }

    if (!display::manager::Device) {
        LOG_ERROR() << "Display device not initialized" << std::endl;
        return;
    }

    auto availableDisplays = display::manager::getAvailableDisplays();
    if (availableDisplays.empty()) {
        LOG_ERROR() << "No available displays" << std::endl;
        return;
    }

    primaryConnector = availableDisplays[0];
    if (!primaryConnector->isConnected()) {
        LOG_ERROR() << "Primary display is not connected" << std::endl;
        return;
    }

    auto modes = primaryConnector->getAvailableModes();
    if (modes.empty()) {
        LOG_ERROR() << "No display modes found" << std::endl;
        return;
    }

    currentMode = std::make_unique<display::mode>(primaryConnector->getPreferredMode());
    framebufferResolution = {static_cast<int>(currentMode->getWidth()), static_cast<int>(currentMode->getHeight())};

    if (!display::manager::enableDisplay(primaryConnector, *currentMode)) {
        LOG_ERROR() << "Failed to enable display" << std::endl;
        return;
    }

    if (!gpuContext.initialize(*display::manager::Device, *currentMode)) {
        LOG_ERROR() << "Failed to initialize GPU context" << std::endl;
        return;
    }

    updateProjectionMatrix(currentMode->getWidth(), currentMode->getHeight());

    if (!createShaderProgram()) {
        LOG_ERROR() << "Failed to create shader program" << std::endl;
        gpuContext.cleanup(*display::manager::Device);
        return;
    }

    glUseProgram(shaderProgram.program);
    glUniformMatrix4fv(shaderProgram.uniformProjection, 1, GL_FALSE, projectionMatrix);
    glUniform1i(shaderProgram.uniformTexture, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    uploadWallpaperTextureIfNeeded();
    registerPageFlipHook();

    shouldExit = false;
    std::thread renderingThread([]() {
        size_t framesRendered = 0;
        auto lastLog = std::chrono::high_resolution_clock::now();

        while (!shouldExit) {
            uploadWallpaperTextureIfNeeded();

            uint32_t bgColor = config::manager::getBackgroundColor();
            float r = static_cast<float>((bgColor >> 16) & 0xFF) / 255.0f;
            float g = static_cast<float>((bgColor >> 8) & 0xFF) / 255.0f;
            float blue = static_cast<float>(bgColor & 0xFF) / 255.0f;
            glViewport(0, 0, framebufferResolution.x, framebufferResolution.y);
            glClearColor(r, g, blue, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            bool frameDrawn = false;
            std::unordered_set<const window::handle*> activeHandles;

            window::manager::handles([&](std::vector<window::handle>& handles) {
                std::sort(handles.begin(), handles.end(), [](const window::handle& a, const window::handle& other) {
                    types::rectangle aRect = a.getCellCoordinates();
                    types::rectangle bRect = other.getCellCoordinates();
                    return aRect.position.z < bRect.position.z;
                });

                renderWallpaper();

                for (auto& handle : handles) {
                    activeHandles.insert(&handle);
                    if (renderHandleInternal(&handle)) {
                        frameDrawn = true;
                    }
                }
            });

            removeStaleHandleResources(activeHandles);
            window::manager::cleanupDeadHandles();
            display::manager::processEvents(0);

            if (frameDrawn || wallpaperReady) {
                auto pendingFrame = gpuContext.swapBuffers(*display::manager::Device);
                if (pendingFrame.framebuffer) {
                    if (!display::manager::present(primaryConnector, pendingFrame.framebuffer)) {
                        gpuContext.releaseFrame(*display::manager::Device, pendingFrame);
                    } else {
                        framesRendered++;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLog);
            if (elapsed.count() >= 5) {
                LOG_VERBOSE() << "Renderer output " << framesRendered << " frames in " << elapsed.count() << " seconds" << std::endl;
                framesRendered = 0;
                lastLog = now;
            }
        }

        LOG_VERBOSE() << "Renderer thread exiting" << std::endl;
    });

    renderingThread.detach();
    rendererInitialized = true;
    LOG_INFO() << "Renderer initialized using GPU path" << std::endl;
}

void exit() {
    if (!rendererInitialized) {
        return;
    }

    LOG_VERBOSE() << "Shutting down renderer" << std::endl;
    shouldExit = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    releaseAllHandleResources();
    destroyWallpaperTexture();
    destroyQuadBuffer();
    destroyShaderProgram();

    if (display::manager::Device) {
        gpuContext.cleanup(*display::manager::Device);
    }

    rendererInitialized = false;
    LOG_VERBOSE() << "Renderer shutdown complete" << std::endl;
}

} // namespace renderer