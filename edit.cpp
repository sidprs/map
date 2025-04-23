#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <future>
#include <mutex>
#include <unordered_map>
#include <tuple>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// stb_image for PNG decoding
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// =====================================================================
// MemoryStruct + WriteMemoryCallback (for libcurl downloads)
// =====================================================================
struct MemoryStruct {
    char* memory;
    size_t size;
    int index;
};

static size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realSize = size * nmemb;
    auto* mem = static_cast<MemoryStruct*>(userp);
    mem->memory = static_cast<char*>(realloc(mem->memory, mem->size + realSize + 1));
    if (!mem->memory) return 0;
    memcpy(&mem->memory[mem->size], contents, realSize);
    mem->size += realSize;
    mem->memory[mem->size] = '\0';
    return realSize;
}

// =====================================================================
// Raw URL download helper
// =====================================================================
unsigned char* DownloadURL(const char* url, size_t* outSize) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Could not initialize curl\n");
        return nullptr;
    }
    MemoryStruct chunk{ static_cast<char*>(malloc(1)), 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; libcurl)");
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl failed for %s: %s\n", url, curl_easy_strerror(res));
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return nullptr;
    }
    curl_easy_cleanup(curl);
    *outSize = chunk.size;
    return reinterpret_cast<unsigned char*>(chunk.memory);
}

// =====================================================================
// TileKey: unique identifier for each tile
// =====================================================================
struct TileKey {
    int z, x, y;
    bool operator==(TileKey const& o) const noexcept {
        return z == o.z && x == o.x && y == o.y;
    }
};

struct TileKeyHash {
    size_t operator()(TileKey const& k) const noexcept {
        return ((static_cast<size_t>(k.z) << 42)
              ^ (static_cast<size_t>(k.x) << 21)
              ^ static_cast<size_t>(k.y));
    }
};

// =====================================================================
// Global tile cache + mutex
// =====================================================================
static std::unordered_map<TileKey, std::vector<unsigned char>, TileKeyHash> g_tileCache;
static std::mutex g_cacheMutex;

// =====================================================================
// Download & decode a single tile, with cache lookup
// =====================================================================
std::vector<unsigned char> LoadAndDecodeTile(int tileX, int tileY, int zoom, int tileSize) {
    TileKey key{zoom, tileX, tileY};
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_tileCache.find(key);
        if (it != g_tileCache.end())
            return it->second;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://tile.openstreetmap.org/%d/%d/%d.png",
             zoom, tileX, tileY);

    size_t dataSize;
    unsigned char* raw = DownloadURL(url, &dataSize);
    std::vector<unsigned char> pixels(tileSize * tileSize * 4, 255);

    if (raw) {
        int w, h;
        unsigned char* img = stbi_load_from_memory(raw, static_cast<int>(dataSize), &w, &h, nullptr, 4);
        free(raw);
        if (img) {
            pixels.assign(img, img + w * h * 4);
            stbi_image_free(img);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_tileCache.emplace(key, pixels);
    }
    return pixels;
}

// =====================================================================
// CompositeMap: parallel fetch + cache + composite
// =====================================================================
std::vector<unsigned char> CompositeMap(int finalWidth, int finalHeight,
                                        double centerLat, double centerLon,
                                        int zoom, int tileSize) {
    double n = pow(2.0, zoom);
    double centerX = (centerLon + 180.0) / 360.0 * n * tileSize;
    double latRad  = centerLat * M_PI / 180.0;
    double centerY = (1 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n * tileSize;

    double topLeftX = centerX - finalWidth / 2.0;
    double topLeftY = centerY - finalHeight / 2.0;

    int startTileX = static_cast<int>(floor(topLeftX / tileSize));
    int startTileY = static_cast<int>(floor(topLeftY / tileSize));
    int endTileX   = static_cast<int>(floor((topLeftX + finalWidth) / tileSize));
    int endTileY   = static_cast<int>(floor((topLeftY + finalHeight) / tileSize));
    int numTilesX  = endTileX - startTileX + 1;
    int numTilesY  = endTileY - startTileY + 1;

    std::vector<unsigned char> finalImage(finalWidth * finalHeight * 4, 255);
    std::vector<std::future<std::vector<unsigned char>>> futures;
    futures.reserve(numTilesX * numTilesY);

    // Launch fetch/decode tasks
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            int x = startTileX + tx;
            int y = startTileY + ty;
            futures.emplace_back(
                std::async(std::launch::async, LoadAndDecodeTile, x, y, zoom, tileSize)
            );
        }
    }

    // Composite tiles as they become ready
    for (int idx = 0; idx < static_cast<int>(futures.size()); ++idx) {
        int ty = idx / numTilesX;
        int tx = idx % numTilesX;
        auto pixels = futures[idx].get();

        int offsetX = static_cast<int>(floor(tx * static_cast<double>(tileSize) - fmod(topLeftX, tileSize)));
        int offsetY = static_cast<int>(floor(ty * static_cast<double>(tileSize) - fmod(topLeftY, tileSize)));

        for (int j = 0; j < tileSize; ++j) {
            int destY = offsetY + j;
            if (destY < 0 || destY >= finalHeight) continue;
            for (int i = 0; i < tileSize; ++i) {
                int destX = offsetX + i;
                if (destX < 0 || destX >= finalWidth) continue;
                int srcIdx  = (j * tileSize + i) * 4;
                int dstIdx  = (destY * finalWidth + destX) * 4;
                memcpy(&finalImage[dstIdx], &pixels[srcIdx], 4);
            }
        }
    }
    return finalImage;
}

// =====================================================================
// Create OpenGL texture from RGBA buffer (unchanged)
// =====================================================================
unsigned int CreateTextureFromImage(const std::vector<unsigned char>& image, int width, int height) {
    unsigned int texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

// =====================================================================
// GLFW error callback
// =====================================================================
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// =====================================================================
// Main: setup GLFW/ImGui, interactive panning/zooming with arrows
// =====================================================================
int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1024, 768, "map visualizer for car", NULL, NULL);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    const int finalWidth = 1000;
    const int finalHeight = 1000;
    int zoom = 16;
    int tileSize = 256;
    double mapCenterLat = 33.4251;
    double mapCenterLon = -111.9400;

    auto composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
    unsigned int mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);

    double lastCenterLat = mapCenterLat;
    double lastCenterLon = mapCenterLon;
    int lastZoom = zoom;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Arrow key panning
        const double panPixels = 50.0;
        double lonStep = panPixels * (360.0 / (tileSize * (1 << zoom)));
        double latStep = panPixels * (180.0 / (tileSize * (1 << zoom)));
        bool moved = false;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)    { mapCenterLon -= lonStep; moved = true; }
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)   { mapCenterLon += lonStep; moved = true; }
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)      { mapCenterLat += latStep; moved = true; }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)    { mapCenterLat -= latStep; moved = true; }
        if (moved) {
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Map Viewer ---
        ImGui::Begin("Map Viewer");
        ImVec2 region = ImGui::GetContentRegionAvail();
        if (region.x < 1 || region.y < 1)
            region = ImVec2((float)finalWidth, (float)finalHeight);
        ImGui::InvisibleButton("MapArea", region);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            double dLon = drag.x * (360.0 / (tileSize * (1 << zoom)));
            double dLat = -drag.y * (180.0 / (tileSize * (1 << zoom)));
            mapCenterLon += dLon;
            mapCenterLat += dLat;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }

        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            zoom = std::clamp(zoom + (wheel > 0 ? 1 : -1), 1, 19);
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }

        ImGui::SetCursorPos(ImVec2((region.x - finalWidth)/2, (region.y - finalHeight)/2));
        ImGui::Image((ImTextureID)mapTexture, ImVec2((float)finalWidth, (float)finalHeight));
        ImGui::End();

        // --- Map Controls ---
        ImGui::Begin("Map Controls");
        ImGui::InputDouble("Center Latitude", &mapCenterLat, 0.0001, 0.001);
        ImGui::InputDouble("Center Longitude", &mapCenterLon, 0.0001, 0.001);
        ImGui::SliderInt("Zoom", &zoom, 1, 19);
        if (ImGui::Button("Update Map") || fabs(mapCenterLat - lastCenterLat) > 1e-6 || fabs(mapCenterLon - lastCenterLon) > 1e-6 || zoom != lastZoom) {
            lastCenterLat = mapCenterLat;
            lastCenterLon = mapCenterLon;
            lastZoom = zoom;
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }
        ImGui::End();

        static bool ScrollX;

    if(ImGui::Begin("Test Window")){
        ImGui::Checkbox("ScrollX", &ScrollX);

        for (int it = 0; it < 4; it++){
            ImGui::PushID(it);

            if(ImGui::BeginTable("test", 2, ScrollX ? ImGuiTableFlags_ScrollX : 0)) {
                ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("0");
                ImGui::TableSetColumnIndex(1); ImGui::Text("1");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("0");
                ImGui::TableSetColumnIndex(1); ImGui::Text("1");

                ImGui::EndTable();
              }

              ImGui::PopID();
          }
      }

      ImGui::End();
    



        // --- Saved Locations ---
        ImGui::Begin("Saved Locations");
        if (ImGui::BeginTable("Locations", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Latitude");
            ImGui::TableSetupColumn("Longitude");
            ImGui::TableHeadersRow();
            struct Location { std::string name; double lat, lon; };
            std::vector<Location> savedLocations = {
                {"Tempe, AZ", 33.4251, -111.9400},
                {"New York, NY", 40.7128,  -74.0060},
                {"San Francisco, CA", 37.7749,  -122.4194},
                {"London, UK", 51.5074,  -0.1278}
            };
            for (size_t i = 0; i < savedLocations.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", savedLocations[i].name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.4f", savedLocations[i].lat);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.4f", savedLocations[i].lon);
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Button(("Select##" + std::to_string(i)).c_str())) {
                    mapCenterLat = savedLocations[i].lat;
                    mapCenterLon = savedLocations[i].lon;
                    composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
                    glDeleteTextures(1, &mapTexture);
                    mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // Rendering
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    glDeleteTextures(1, &mapTexture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

