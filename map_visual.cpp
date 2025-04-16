/*
Author  : Sid Prasad
Purpose : Create navigation system to pair with traffic recognition
          scripts in order to create finite state machine diagrams.
          End goal is to create a embedded autonomous navigation system.

TODO    : Fix render of map
        : Fix latency issues
        : Implement directions (GPS Module)
        : Integrate sensor information using imgui
*/

#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// stb_image for download and decoding PNG tiles of the map
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ===============================================================
// Structures and callback for downloading data via libcurl.
// ===============================================================
struct MemoryStruct {
    char* memory;               // char* to store bytes
    size_t size;                // size of data
};

static size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realSize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;
    mem->memory = (char*)realloc(mem->memory, mem->size + realSize + 1);
    if (!mem->memory) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }
    memcpy(&(mem->memory[mem->size]), contents, realSize);
    mem->size += realSize;
    mem->memory[mem->size] = 0;
    return realSize;
}

// ----------------------------------------------------------------
// Download the content of a URL into a memory buffer.
unsigned char* DownloadURL(const char* url, size_t* outSize) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Could not initialize curl\n");
        return nullptr;
    }
    MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; libcurl)");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed for URL %s: %s\n", url, curl_easy_strerror(res));
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return nullptr;
    }
    curl_easy_cleanup(curl);
    *outSize = chunk.size;
    return (unsigned char*)chunk.memory;
}

// ----------------------------------------------------------------
// Download a tile image from URL and decode using stb_image.
// Returns a pointer to RGBA image data (to be freed by stbi_image_free)
// and sets outWidth/outHeight.
unsigned char* LoadTileImage(const char* url, int* outWidth, int* outHeight) {
    size_t dataSize = 0;
    unsigned char* data = DownloadURL(url, &dataSize);
    if (!data) return nullptr;
    unsigned char* img = stbi_load_from_memory(data, (int)dataSize, outWidth, outHeight, nullptr, 4);
    free(data);
    if (!img) {
        fprintf(stderr, "Failed to decode image from %s\n", url);
    }
    return img;
}

// ----------------------------------------------------------------
// Helper functions to compute OSM tile indices.
int LonToTileX(double lon_deg, int zoom) {
    return (int)floor((lon_deg + 180.0) / 360.0 * (1 << zoom));
}
int LatToTileY(double lat_deg, int zoom) {
    double lat_rad = lat_deg * M_PI / 180.0;
    return (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * (1 << zoom));
}

// ----------------------------------------------------------------
// GLFW error callback.
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// ----------------------------------------------------------------
// CompositeMap: Download necessary OSM tiles, composite into a 600x400 image.
// Returns an RGBA buffer (as vector<unsigned char>).
std::vector<unsigned char> CompositeMap(int finalWidth, int finalHeight,
                                        double centerLat, double centerLon,
                                        int zoom, int tileSize) {
    double n = pow(2.0, zoom);
    double centerX = (centerLon + 180.0) / 360.0 * n * tileSize;
    double latRad = centerLat * M_PI / 180.0;
    double centerY = (1 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n * tileSize;

    double topLeftX = centerX - finalWidth / 2.0;
    double topLeftY = centerY - finalHeight / 2.0;

    int startTileX = (int)floor(topLeftX / tileSize);
    int startTileY = (int)floor(topLeftY / tileSize);
    int endTileX   = (int)floor((topLeftX + finalWidth) / tileSize);
    int endTileY   = (int)floor((topLeftY + finalHeight) / tileSize);
    int numTilesX  = endTileX - startTileX + 1;
    int numTilesY  = endTileY - startTileY + 1;

    std::vector<std::vector<unsigned char*>> tileImages(numTilesY,
        std::vector<unsigned char*>(numTilesX, nullptr));
    char url[512];

    for (int ty = 0; ty < numTilesY; ty++) {
        for (int tx = 0; tx < numTilesX; tx++) {
            int tileX = startTileX + tx;
            int tileY = startTileY + ty;
            snprintf(url, sizeof(url),
                     "https://tile.openstreetmap.org/%d/%d/%d.png",
                     zoom, tileX, tileY);
            int w, h;
            unsigned char* img = LoadTileImage(url, &w, &h);
            if (!img) fprintf(stderr, "Failed to load tile: %s\n", url);
            tileImages[ty][tx] = img;
        }
    }

    std::vector<unsigned char> finalImage(finalWidth * finalHeight * 4, 255);
    for (int j = 0; j < finalHeight; j++) {
        for (int i = 0; i < finalWidth; i++) {
            double gx = topLeftX + i;
            double gy = topLeftY + j;
            int col = (int)floor(gx / tileSize) - startTileX;
            int row = (int)floor(gy / tileSize) - startTileY;
            int px  = ((int)gx) % tileSize;
            int py  = ((int)gy) % tileSize;
            if (row >= 0 && row < numTilesY && col >= 0 && col < numTilesX) {
                unsigned char* tileImg = tileImages[row][col];
                if (tileImg) {
                    int ti = (py * tileSize + px) * 4;
                    int fi = (j * finalWidth + i) * 4;
                    memcpy(&finalImage[fi], &tileImg[ti], 4);
                }
            }
        }
    }

    // Free tiles
    for (int ty = 0; ty < numTilesY; ty++)
        for (int tx = 0; tx < numTilesX; tx++)
            if (tileImages[ty][tx])
                stbi_image_free(tileImages[ty][tx]);

    return finalImage;
}

// ----------------------------------------------------------------
// Upload RGBA buffer as an OpenGL texture.
unsigned int CreateTextureFromImage(const std::vector<unsigned char>& image,
                                    int width, int height) {
    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// ----------------------------------------------------------------
// Saved locations structure.
struct Location {
    std::string name;
    double lat, lon;
};

// ----------------------------------------------------------------
// --- NEW FUNCTION: Smoothly interpolate map parameters & re-render ---
//
// Interpolates current mapCenterLat/Lon and zoom toward targetLat/Lon/zoom,
// then, if there's a noticeable movement, recomputes the composite map
// (throttled per frame) so the user sees a smooth pan/zoom.
void SmoothUpdateMap(int finalWidth, int finalHeight, int tileSize,
                     std::vector<unsigned char>& composite,
                     unsigned int& mapTexture,
                     double& mapCenterLat, double& mapCenterLon,
                     int& zoom,
                     double targetLat, double targetLon, int targetZoom) {
    const double t = 0.2; // interpolation factor

    bool moved = false;
    // Lerp latitude
    if (fabs(mapCenterLat - targetLat) > 1e-6) {
        mapCenterLat += (targetLat - mapCenterLat) * t;
        moved = true;
    }
    // Lerp longitude
    if (fabs(mapCenterLon - targetLon) > 1e-6) {
        mapCenterLon += (targetLon - mapCenterLon) * t;
        moved = true;
    }
    // Step zoom one level at a time
    if (zoom < targetZoom) {
        zoom++;
        moved = true;
    } else if (zoom > targetZoom) {
        zoom--;
        moved = true;
    }

    if (moved) {
        // Rebuild the composite and texture
        composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
        glDeleteTextures(1, &mapTexture);
        mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
    }
}

// ----------------------------------------------------------------
// Main entrypoint
int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE,   GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1024, 768, "map visualizer for car", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Map parameters & texture
    const int finalWidth  = 600;
    const int finalHeight = 400;
    const int tileSize    = 256;
    double mapCenterLat = 33.4251, mapCenterLon = -111.9400;
    int zoom = 16;

    // Targets start equal to current
    double targetLat = mapCenterLat, targetLon = mapCenterLon;
    int    targetZoom = zoom;

    // Initial composite + texture
    auto composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
    unsigned int mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);

    // Saved locations
    std::vector<Location> savedLocations = {
        {"Tempe, AZ",      33.4251,  -111.9400},
        {"New York, NY",   40.7128,   -74.0060},
        {"San Francisco",  37.7749,  -122.4194},
        {"London, UK",     51.5074,    -0.1278},
        {"Chandler, AZ",   33.3062,  -111.8413},
        {"Villas On Apach",33.4210,  -111.9100}
    };

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Map Viewer ---
        ImGui::Begin("Map Viewer");
        ImVec2 mapRegion = ImGui::GetContentRegionAvail();
        if (mapRegion.x<1||mapRegion.y<1)
            mapRegion = ImVec2((float)finalWidth,(float)finalHeight);
        ImGui::InvisibleButton("MapArea", mapRegion);

        // Handle dragging: update targetLat/Lon, not immediate render
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            // Convert pixel drag to geo delta
            double dLon = drag.x * (360.0 / (tileSize * (1<<zoom)));
            double dLat = -drag.y * (180.0 / (tileSize * (1<<zoom)));
            targetLon += dLon;
            targetLat += dLat;
        }

        // Mouse wheel zoom => update targetZoom
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel!=0.0f) {
            targetZoom += (wheel>0?1:-1);
            if (targetZoom<1) targetZoom=1;
            if (targetZoom>19) targetZoom=19;
        }

        // Center texture on panel
        ImGui::SetCursorPos(ImVec2(
            (mapRegion.x-finalWidth)/2,
            (mapRegion.y-finalHeight)/2
        ));
        ImGui::Image((ImTextureID)mapTexture, ImVec2((float)finalWidth,(float)finalHeight));
        ImGui::End();

        // --- Controls: manual override + “Update Map” button ---
        ImGui::Begin("Map Controls");
        ImGui::InputDouble("Center Lat", &targetLat, 0.0001, 0.001);
        ImGui::InputDouble("Center Lon", &targetLon, 0.0001, 0.001);
        ImGui::SliderInt("Zoom", &targetZoom, 1, 19);
        if (ImGui::Button("Update Map")) {
            // nothing else needed: SmoothUpdateMap will pick up new targets
        }
        ImGui::End();

        // --- Saved Locations ---
        ImGui::Begin("Saved Locations");
        if (ImGui::BeginTable("Locations", 3, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Lat");
            ImGui::TableSetupColumn("Lon");
            ImGui::TableHeadersRow();
            for (int i=0; i<savedLocations.size(); i++) {
                auto &loc = savedLocations[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", loc.name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.4f", loc.lat);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.4f", loc.lon);
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Button((std::string("Select##")+std::to_string(i)).c_str())) {
                    targetLat = loc.lat;
                    targetLon = loc.lon;
                    // targetZoom remains
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // --- SMOOTH PAN / ZOOM update (calls CompositeMap when needed) ---
        SmoothUpdateMap(finalWidth, finalHeight, tileSize,
                        composite, mapTexture,
                        mapCenterLat, mapCenterLon,
                        zoom,
                        targetLat, targetLon, targetZoom);

        // --- Rendering ---
        ImGui::Render();
        int w,h; glfwGetFramebufferSize(window,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0.1f,0.1f,0.1f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    glDeleteTextures(1, &mapTexture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

