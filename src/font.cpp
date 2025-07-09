#include "font.h"
#include "logger.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <filesystem>

// FreeType includes
#include <ft2build.h>
#include FT_FREETYPE_H

namespace font {

    // UTF-8 to UTF-32 conversion
    char32_t font::utf8ToUtf32(const char utf8[4]) {
        char32_t codepoint = 0;
        
        // Handle null or empty input
        if (!utf8 || utf8[0] == 0) {
            return 0;
        }
        
        // Single byte (ASCII)
        if ((utf8[0] & 0x80) == 0) {
            codepoint = utf8[0];
        }
        // Two bytes
        else if ((utf8[0] & 0xE0) == 0xC0) {
            codepoint = ((utf8[0] & 0x1F) << 6) | (utf8[1] & 0x3F);
        }
        // Three bytes
        else if ((utf8[0] & 0xF0) == 0xE0) {
            codepoint = ((utf8[0] & 0x0F) << 12) | ((utf8[1] & 0x3F) << 6) | (utf8[2] & 0x3F);
        }
        // Four bytes
        else if ((utf8[0] & 0xF8) == 0xF0) {
            codepoint = ((utf8[0] & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) | ((utf8[2] & 0x3F) << 6) | (utf8[3] & 0x3F);
        }
        
        return codepoint;
    }

    std::vector<char32_t> font::utf8StringToUtf32(const std::string& utf8) {
        std::vector<char32_t> result;
        size_t i = 0;
        
        while (i < utf8.length()) {
            char utf8Char[4] = {0};
            
            // Determine the number of bytes for this character
            int numBytes = 1;
            if ((utf8[i] & 0xE0) == 0xC0) numBytes = 2;
            else if ((utf8[i] & 0xF0) == 0xE0) numBytes = 3;
            else if ((utf8[i] & 0xF8) == 0xF0) numBytes = 4;
            
            // Copy the bytes
            for (int j = 0; j < numBytes && i + j < utf8.length(); j++) {
                utf8Char[j] = utf8[i + j];
            }
            
            // Convert to UTF-32
            char32_t codepoint = utf8ToUtf32(utf8Char);
            if (codepoint > 0) {
                result.push_back(codepoint);
            }
            
            i += numBytes;
        }
        
        return result;
    }

    // Font class implementation
    font::font(const std::string& FontPath, int FontSize) 
        : fontPath(FontPath), fontSize(FontSize), lineHeight(0), maxWidth(0), loaded(false),
          ftLibrary(nullptr), ftFace(nullptr) {
        
        if (initializeFreeType() && loadFontFile()) {
            loaded = true;
        }
    }

    font::~font() {
        cleanupFreeType();
    }

    font::font(font&& other) noexcept 
        : fontPath(std::move(other.fontPath)), fontSize(other.fontSize), 
          lineHeight(other.lineHeight), maxWidth(other.maxWidth), loaded(other.loaded),
          ftLibrary(other.ftLibrary), ftFace(other.ftFace),
          glyphCache(std::move(other.glyphCache)) {
        
        other.ftLibrary = nullptr;
        other.ftFace = nullptr;
        other.loaded = false;
    }

    font& font::operator=(font&& other) noexcept {
        if (this != &other) {
            cleanupFreeType();
            
            fontPath = std::move(other.fontPath);
            fontSize = other.fontSize;
            lineHeight = other.lineHeight;
            maxWidth = other.maxWidth;
            loaded = other.loaded;
            ftLibrary = other.ftLibrary;
            ftFace = other.ftFace;
            glyphCache = std::move(other.glyphCache);
            
            other.ftLibrary = nullptr;
            other.ftFace = nullptr;
            other.loaded = false;
        }
        return *this;
    }

    bool font::initializeFreeType() {
        FT_Library* library = reinterpret_cast<FT_Library*>(&ftLibrary);
        FT_Error error = FT_Init_FreeType(library);
        
        if (error) {
            LOG_ERROR() << "Could not initialize FreeType library: " << error << std::endl;
            return false;
        }
        
        return true;
    }

    void font::cleanupFreeType() {
        if (ftFace) {
            FT_Done_Face(*reinterpret_cast<FT_Face*>(&ftFace));
            ftFace = nullptr;
        }
        
        if (ftLibrary) {
            FT_Done_FreeType(*reinterpret_cast<FT_Library*>(&ftLibrary));
            ftLibrary = nullptr;
        }
    }

    bool font::loadFontFile() {
        if (!ftLibrary) {
            return false;
        }
        
        FT_Library library = *reinterpret_cast<FT_Library*>(&ftLibrary);
        FT_Face* face = reinterpret_cast<FT_Face*>(&ftFace);
        
        FT_Error error = FT_New_Face(library, fontPath.c_str(), 0, face);
        
        if (error == FT_Err_Unknown_File_Format) {
            LOG_ERROR() << "Font file format not supported: " << fontPath << std::endl;
            return false;
        } else if (error) {
            LOG_ERROR() << "Could not load font file: " << fontPath << " Error: " << error << std::endl;
            return false;
        }
        
        // Set font size
        error = FT_Set_Pixel_Sizes(*face, 0, fontSize);
        if (error) {
            LOG_ERROR() << "Could not set font size: " << error << std::endl;
            return false;
        }
        
        // Calculate line height and max width
        FT_Face faceHandle = *face;
        lineHeight = faceHandle->size->metrics.height >> 6;
        maxWidth = faceHandle->size->metrics.max_advance >> 6;
        
        return true;
    }

    glyph font::getGlyph(char32_t codepoint) {
        // Check cache first
        auto it = glyphCache.find(codepoint);
        if (it != glyphCache.end()) {
            return it->second;
        }
        
        // Load glyph if not in cache
        if (loadGlyph(codepoint)) {
            return glyphCache[codepoint];
        }
        
        // Return empty glyph if loading failed
        return glyph{};
    }

    bool font::loadGlyph(char32_t codepoint) {
        if (!ftFace) {
            return false;
        }
        
        FT_Face face = *reinterpret_cast<FT_Face*>(&ftFace);
        
        // Load the glyph
        FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (glyphIndex == 0) {
            // Glyph not found, use replacement character or space
            glyphIndex = FT_Get_Char_Index(face, 0x20); // Space character
            if (glyphIndex == 0) {
                return false;
            }
        }
        
        FT_Error error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT);
        if (error) {
            LOG_ERROR() << "Could not load glyph for codepoint: " << codepoint << std::endl;
            return false;
        }
        
        // Render the glyph to a bitmap
        error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
        if (error) {
            LOG_ERROR() << "Could not render glyph for codepoint: " << codepoint << std::endl;
            return false;
        }
        
        // Create glyph structure
        glyph newGlyph;
        newGlyph.codepoint = codepoint;
        newGlyph.width = face->glyph->bitmap.width;
        newGlyph.height = face->glyph->bitmap.rows;
        newGlyph.bearingX = face->glyph->bitmap_left;
        newGlyph.bearingY = face->glyph->bitmap_top;
        newGlyph.advance = face->glyph->advance.x >> 6;
        
        // Copy bitmap data
        size_t bitmapSize = newGlyph.width * newGlyph.height;
        newGlyph.bitmap.resize(bitmapSize);
        
        if (bitmapSize > 0) {
            std::memcpy(newGlyph.bitmap.data(), face->glyph->bitmap.buffer, bitmapSize);
        }
        
        // Cache the glyph
        glyphCache[codepoint] = newGlyph;
        
        return true;
    }

    cellRenderData font::renderCell(const types::Cell& cell, cellRenderData& cellData, float zoom) {
        // Convert UTF-8 to UTF-32
        char32_t codepoint = utf8ToUtf32(cell.utf);
        
        // Skip rendering if it's a null character or space
        if (codepoint == 0 || codepoint == 0x20) {
            return cellData;
        }
        
        // Get glyph
        glyph glyphData = getGlyph(codepoint);
        if (glyphData.bitmap.empty()) {
            return cellData;
        }
        
        // Render glyph to cell
        renderGlyphToCell(glyphData, cellData, cell.textColor, cellData.width, cellData.height, zoom);
        
        return cellData;
    }

    void font::renderGlyphToCell(const glyph& glyph, cellRenderData& cellData, const types::RGB& foreground, int cellWidth, int cellHeight, float zoom) {
        
        if (glyph.bitmap.empty()) {
            return;
        }
        
        // Calculate glyph position within the cell
        int glyphScaledWidth = static_cast<int>(glyph.width * zoom);
        int glyphScaledHeight = static_cast<int>(glyph.height * zoom);
        
        // Center the glyph horizontally and align to baseline vertically
        int startX = (cellWidth - glyphScaledWidth) / 2;
        int startY = static_cast<int>((cellHeight * 0.8f) - (glyph.bearingY * zoom)); // Approximate baseline
        
        // Ensure we don't go out of bounds
        startX = std::max(0, std::min(startX, cellWidth - glyphScaledWidth));
        startY = std::max(0, std::min(startY, cellHeight - glyphScaledHeight));
        
        // Render each pixel of the glyph
        for (int y = 0; y < glyphScaledHeight && (startY + y) < cellHeight; y++) {
            for (int x = 0; x < glyphScaledWidth && (startX + x) < cellWidth; x++) {
                // Sample from the original glyph bitmap
                int srcX = static_cast<int>(x / zoom);
                int srcY = static_cast<int>(y / zoom);
                
                if (srcX >= glyph.width || srcY >= glyph.height) {
                    continue;
                }
                
                int srcIndex = srcY * glyph.width + srcX;
                uint8_t alpha = glyph.bitmap[srcIndex];
                
                if (alpha > 0) {
                    // Calculate destination pixel position
                    int dstX = startX + x;
                    int dstY = startY + y;
                    int dstIndex = dstY * cellWidth + dstX;
                    
                    if (dstIndex < (signed)cellData.pixels.size()) {
                        // Alpha blend the foreground color
                        float alphaF = alpha / 255.0f;
                        float invAlphaF = 1.0f - alphaF;
                        
                        types::RGB& dstPixel = cellData.pixels[dstIndex];
                        dstPixel.r = static_cast<uint8_t>(foreground.r * alphaF + dstPixel.r * invAlphaF);
                        dstPixel.g = static_cast<uint8_t>(foreground.g * alphaF + dstPixel.g * invAlphaF);
                        dstPixel.b = static_cast<uint8_t>(foreground.b * alphaF + dstPixel.b * invAlphaF);
                    }
                }
            }
        }
    }

    // Font manager implementation
    namespace manager {
        std::shared_ptr<font> defaultFont;
        std::unordered_map<std::string, std::shared_ptr<font>> fontRegistry;
        std::string configurableFontName = "default";
        int defaultCellWidth = 6;
        int defaultCellHeight = 12;

        std::vector<std::string> getSystemFontPaths() {
            std::vector<std::string> paths;
            
            // Common system font directories
            std::vector<std::string> fontDirs = {
                "/usr/share/fonts/",
                "/usr/local/share/fonts/",
                "/System/Library/Fonts/",
                "/Library/Fonts/",
                "~/.fonts/",
                "~/.local/share/fonts/"
            };
            
            for (const auto& dir : fontDirs) {
                if (std::filesystem::exists(dir)) {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
                        if (entry.is_regular_file()) {
                            std::string extension = entry.path().extension().string();
                            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                            
                            if (extension == ".ttf" || extension == ".otf" || extension == ".woff" || extension == ".woff2") {
                                paths.push_back(entry.path().string());
                            }
                        }
                    }
                }
            }
            
            return paths;
        }

        std::string findSystemFont() {
            std::vector<std::string> fontPaths = getSystemFontPaths();
            
            // Preferred font names (in order of preference)
            std::vector<std::string> preferredFonts = {
                "DejaVuSansMono",
                "Liberation Mono",
                "Consolas",
                "Courier New",
                "Menlo",
                "Monaco",
                "Ubuntu Mono",
                "Fira Code",
                "Source Code Pro"
            };
            
            // Look for preferred fonts
            for (const auto& preferred : preferredFonts) {
                for (const auto& path : fontPaths) {
                    std::string filename = std::filesystem::path(path).filename().string();
                    if (filename.find(preferred) != std::string::npos) {
                        return path;
                    }
                }
            }
            
            // Fall back to first monospace font found
            for (const auto& path : fontPaths) {
                std::string filename = std::filesystem::path(path).filename().string();
                std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
                
                if (filename.find("mono") != std::string::npos || 
                    filename.find("courier") != std::string::npos ||
                    filename.find("console") != std::string::npos) {
                    return path;
                }
            }
            
            // Last resort - return first font found
            if (!fontPaths.empty()) {
                return fontPaths[0];
            }
            
            return "";
        }

        bool initialize(const std::string& defaultFontPath, int defaultFontSize) {
            std::string fontPath = defaultFontPath;
            
            if (fontPath.empty()) {
                fontPath = findSystemFont();
            }
            
            if (fontPath.empty()) {
                LOG_ERROR() << "No suitable font found on system" << std::endl;
                return false;
            }
            
            try {
                defaultFont = std::make_shared<font>(fontPath, defaultFontSize);
                if (!defaultFont->isLoaded()) {
                    LOG_ERROR() << "Failed to load default font: " << fontPath << std::endl;
                    return false;
                }
                
                // Register as default font
                fontRegistry["default"] = defaultFont;
                
                LOG_INFO() << "Font system initialized with: " << fontPath << std::endl;
                return true;
                
            } catch (const std::exception& e) {
                LOG_ERROR() << "Error initializing font system: " << e.what() << std::endl;
                return false;
            }
        }

        void cleanup() {
            defaultFont.reset();
            fontRegistry.clear();
        }

        std::shared_ptr<font> getDefaultFont() {
            return defaultFont;
        }

        std::shared_ptr<font> getFont(const std::string& fontName) {
            auto it = fontRegistry.find(fontName);
            if (it != fontRegistry.end()) {
                return it->second;
            }
            return defaultFont;
        }

        bool setDefaultFont(const std::string& fontPath, int fontSize) {
            try {
                auto newFont = std::make_shared<font>(fontPath, fontSize);
                if (!newFont->isLoaded()) {
                    return false;
                }
                
                defaultFont = newFont;
                fontRegistry["default"] = newFont;
                return true;
                
            } catch (const std::exception& e) {
                LOG_ERROR() << "Error setting default font: " << e.what() << std::endl;
                return false;
            }
        }

        bool addFont(const std::string& fontName, const std::string& fontPath, int fontSize) {
            try {
                auto newFont = std::make_shared<font>(fontPath, fontSize);
                if (!newFont->isLoaded()) {
                    return false;
                }
                
                fontRegistry[fontName] = newFont;
                return true;
                
            } catch (const std::exception& e) {
                LOG_ERROR() << "Error adding font: " << e.what() << std::endl;
                return false;
            }
        }

        void setConfigurableFontName(const std::string& fontName) {
            configurableFontName = fontName;
        }

        std::string getConfigurableFontName() {
            return configurableFontName;
        }

        int getDefaultCellWidth() {
            return defaultCellWidth;
        }

        int getDefaultCellHeight() {
            return defaultCellHeight;
        }

        void setDefaultCellSize(int width, int height) {
            defaultCellWidth = width;
            defaultCellHeight = height;
        }
    }

}
