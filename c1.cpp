#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

// GLFW and OpenGL headers.
#include <GLFW/glfw3.h>

// ImGui headers and backends.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// stb_image for downloading and decoding PNG tiles.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ----------------------------------------------------------------
// Structures and callback for downloading data via libcurl.
struct MemoryStruct {
    char* memory;
    size_t size;
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
    return (int)floor((1.0 - log(tan(lat_rad) + 1.0/cos(lat_rad)) / M_PI) / 2.0 * (1 << zoom));
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
    // Global pixel coordinates for center.
    double centerX = (centerLon + 180.0) / 360.0 * n * tileSize;
    double latRad = centerLat * M_PI / 180.0;
    double centerY = (1 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n * tileSize;
    
    double topLeftX = centerX - finalWidth / 2.0;
    double topLeftY = centerY - finalHeight / 2.0;
    
    int startTileX = (int)floor(topLeftX / tileSize);
    int startTileY = (int)floor(topLeftY / tileSize);
    int endTileX = (int)floor((topLeftX + finalWidth) / tileSize);
    int endTileY = (int)floor((topLeftY + finalHeight) / tileSize);
    int numTilesX = endTileX - startTileX + 1;
    int numTilesY = endTileY - startTileY + 1;
    
    // 2D vector for downloaded tiles.
    std::vector<std::vector<unsigned char*>> tileImages(numTilesY, std::vector<unsigned char*>(numTilesX, nullptr));
    
    char url[512];
    for (int ty = 0; ty < numTilesY; ty++) {
        for (int tx = 0; tx < numTilesX; tx++) {
            int tileX = startTileX + tx;
            int tileY = startTileY + ty;
            // Use default OSM tile server (light mode).
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", zoom, tileX, tileY);
            int w, h;
            unsigned char* img = LoadTileImage(url, &w, &h);
            if (!img) {
                fprintf(stderr, "Failed to load tile: %s\n", url);
            }
            tileImages[ty][tx] = img;
        }
    }
    
    // Composite the final image.
    std::vector<unsigned char> finalImage(finalWidth * finalHeight * 4, 255); // white background
    for (int j = 0; j < finalHeight; j++) {
        for (int i = 0; i < finalWidth; i++) {
            double globalX = topLeftX + i;
            double globalY = topLeftY + j;
            int tileCol = (int)floor(globalX / tileSize) - startTileX;
            int tileRow = (int)floor(globalY / tileSize) - startTileY;
            int pixelX = ((int)globalX) % tileSize;
            int pixelY = ((int)globalY) % tileSize;
            if (tileRow >= 0 && tileRow < numTilesY && tileCol >= 0 && tileCol < numTilesX) {
                unsigned char* tileImg = tileImages[tileRow][tileCol];
                if (tileImg) {
                    int idxTile = (pixelY * tileSize + pixelX) * 4;
                    int idxFinal = (j * finalWidth + i) * 4;
                    memcpy(&finalImage[idxFinal], &tileImg[idxTile], 4);
                }
            }
        }
    }
    
    // Free tile images.
    for (int ty = 0; ty < numTilesY; ty++) {
        for (int tx = 0; tx < numTilesX; tx++) {
            if (tileImages[ty][tx])
                stbi_image_free(tileImages[ty][tx]);
        }
    }
    return finalImage;
}

// ----------------------------------------------------------------
// Create an OpenGL texture from an RGBA image buffer.
unsigned int CreateTextureFromImage(const std::vector<unsigned char>& image, int width, int height) {
    unsigned int texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    // Set linear filtering.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

// ----------------------------------------------------------------
// Saved locations structure.
struct Location {
    std::string name;
    double lat;
    double lon;
};

// ----------------------------------------------------------------
// Main function: sets up GLFW and ImGui, allows interactive mouse panning/zooming, and displays a table of saved locations.
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
    
    // Setup ImGui.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Set UI appearance to dark mode.
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Map parameters.
    const int finalWidth = 600;
    const int finalHeight = 400;
    int zoom = 16;
    int tileSize = 256;
    double mapCenterLat = 33.4251;
    double mapCenterLon = -111.9400;
    
    // Create the composite map and texture.
    std::vector<unsigned char> composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
    unsigned int mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
    
    // Variables to check parameter changes.
    double lastCenterLat = mapCenterLat;
    double lastCenterLon = mapCenterLon;
    int lastZoom = zoom;
    
    // For mouse-based panning.
    double panOffsetX = 0.0;
    double panOffsetY = 0.0;
    
    // A sample table of saved locations.
    std::vector<Location> savedLocations = {
        {"Tempe, AZ", 33.4251, -111.9400},
        {"New York, NY", 40.7128, -74.0060},
        {"San Francisco, CA", 37.7749, -122.4194},
        {"London, UK", 51.5074, -0.1278}
    };
    
    // Main loop.
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // --- Map Viewer Window ---
        ImGui::Begin("Map Viewer");
        // Create an invisible region for mouse interaction.
        ImVec2 mapRegion = ImGui::GetContentRegionAvail();
        if (mapRegion.x < 1.0f || mapRegion.y < 1.0f)
            mapRegion = ImVec2((float)finalWidth, (float)finalHeight);
        ImGui::InvisibleButton("MapArea", mapRegion);
        // Get drag delta
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            panOffsetX += dragDelta.x;
            panOffsetY += dragDelta.y;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            // Convert pixel drag to geographic delta.
            double dLon = dragDelta.x * (360.0 / (tileSize * (1 << zoom)));
            double dLat = -dragDelta.y * (180.0 / (tileSize * (1 << zoom)));
            mapCenterLon += dLon;
            mapCenterLat += dLat;
            // Update map immediately.
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }
        // Use mouse wheel for zooming.
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            zoom += (wheel > 0 ? 1 : -1);
            if (zoom < 1) zoom = 1;
            if (zoom > 19) zoom = 19;
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }
        // Center the drawn texture.
        ImGui::SetCursorPos(ImVec2((mapRegion.x - finalWidth)/2, (mapRegion.y - finalHeight)/2));
        ImGui::Image((ImTextureID)mapTexture, ImVec2((float)finalWidth, (float)finalHeight));
        ImGui::End();
        
        // --- Map Controls Window ---
        ImGui::Begin("Map Controls");
        ImGui::InputDouble("Center Latitude", &mapCenterLat, 0.0001, 0.001);
        ImGui::InputDouble("Center Longitude", &mapCenterLon, 0.0001, 0.001);
        ImGui::SliderInt("Zoom", &zoom, 1, 19);
        if (ImGui::Button("Update Map") ||
            fabs(mapCenterLat - lastCenterLat) > 1e-6 ||
            fabs(mapCenterLon - lastCenterLon) > 1e-6 ||
            (zoom != lastZoom)) {
            lastCenterLat = mapCenterLat;
            lastCenterLon = mapCenterLon;
            lastZoom = zoom;
            composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
            glDeleteTextures(1, &mapTexture);
            mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
        }
        ImGui::End();
        
        // --- Saved Locations Window ---
        ImGui::Begin("Saved Locations");
        if (ImGui::BeginTable("Locations", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Latitude");
            ImGui::TableSetupColumn("Longitude");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < savedLocations.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", savedLocations[i].name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.4f", savedLocations[i].lat);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.4f", savedLocations[i].lon);
                // Add a Select button per row.
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Button(("Select##" + std::to_string(i)).c_str())) {
                    mapCenterLat = savedLocations[i].lat;
                    mapCenterLon = savedLocations[i].lon;
                    // Update map immediately.
                    composite = CompositeMap(finalWidth, finalHeight, mapCenterLat, mapCenterLon, zoom, tileSize);
                    glDeleteTextures(1, &mapTexture);
                    mapTexture = CreateTextureFromImage(composite, finalWidth, finalHeight);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
        
        // Rendering.
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Dark background for UI.
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    
    // Cleanup.
    glDeleteTextures(1, &mapTexture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

