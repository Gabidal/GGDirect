#ifndef _FONT_H_
#define _FONT_H_

#include "types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace font {

    struct glyph {
        char32_t codepoint;             // UTF-32 codepoint
        std::vector<uint8_t> bitmap;    // 8-bit grayscale bitmap
        int width;
        int height;
        int bearingX;                   // Horizontal bearing (offset from baseline)
        int bearingY;                   // Vertical bearing (offset from baseline)
        int advance;                    // Horizontal advance to next glyph
    };

    struct cellRenderData {
        int width;
        int height;
        std::vector<types::RGB> pixels; // RGB pixel buffer for the cell
    };

    class font {
    public:
        font(const std::string& fontPath, int fontSize = 16);
        ~font();

        // Disable copy, enable move
        font(const font&) = delete;
        font& operator=(const font&) = delete;
        font(font&&) noexcept;
        font& operator=(font&&) noexcept;

        // Glyph operations
        glyph getGlyph(char32_t codepoint);
        bool loadGlyph(char32_t codepoint);
        
        // Cell rendering - main function for rendering cells
        cellRenderData renderCell(const types::Cell& cell, cellRenderData& cellData, float zoom = 1.0f);
        
        // Utility functions
        int getFontSize() const { return fontSize; }
        int getLineHeight() const { return lineHeight; }
        int getMaxWidth() const { return maxWidth; }
        bool isLoaded() const { return loaded; }
        
        // UTF-8 utilities
        static char32_t utf8ToUtf32(const char utf8[4]);
        static std::vector<char32_t> utf8StringToUtf32(const std::string& utf8);

    private:
        std::string fontPath;
        int fontSize;
        int lineHeight;
        int maxWidth;
        bool loaded;
        
        // FreeType library handle
        void* ftLibrary;
        void* ftFace;
        
        // Glyph cache
        std::unordered_map<char32_t, glyph> glyphCache;
        
        // Private methods
        bool initializeFreeType();
        void cleanupFreeType();
        bool loadFontFile();
        void renderGlyphToCell(const glyph& glyph, cellRenderData& cellData, 
                              const types::RGB& foreground,
                              int cellWidth, int cellHeight, float zoom);
    };

    // Global font manager
    namespace manager {
        // Initialize with default system font
        extern bool initialize(const std::string& defaultFontPath = "", int defaultFontSize = 16);
        extern void cleanup();
        
        // Font management
        extern std::shared_ptr<font> getDefaultFont();
        extern std::shared_ptr<font> getFont(const std::string& fontName);
        extern bool setDefaultFont(const std::string& fontPath, int fontSize = 16);
        extern bool addFont(const std::string& fontName, const std::string& fontPath, int fontSize = 16);
        
        // Configuration
        extern void setConfigurableFontName(const std::string& fontName);
        extern std::string getConfigurableFontName();
        
        // Default cell dimensions
        extern int getDefaultCellWidth();
        extern int getDefaultCellHeight();
        extern void setDefaultCellSize(int width, int height);
        
        // Internal state
        extern std::shared_ptr<font> defaultFont;
        extern std::unordered_map<std::string, std::shared_ptr<font>> fontRegistry;
        extern std::string configurableFontName;
        extern int defaultCellWidth;
        extern int defaultCellHeight;
        
        // Private methods
        extern std::string findSystemFont();
        extern std::vector<std::string> getSystemFontPaths();
    };

}

#endif