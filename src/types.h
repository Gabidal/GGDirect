#ifndef _TYPES_H_
#define _TYPES_H_

#include <cstdint>
#include <algorithm>
#include <iterator>

// Forward declarations to avoid circular includes
namespace font {
    namespace manager {
        extern int getDefaultCellWidth();
        extern int getDefaultCellHeight();
    }
}

namespace types {

    class iVector2 {
    public: 
        int x, y;

        iVector2(int x_ = 0, int y_ = 0) : x(x_), y(y_) {}

        bool operator==(const iVector2& other) const {
            return x == other.x && y == other.y;
        }

        bool operator!=(const iVector2& other) const {
            return !(*this == other);
        }

        iVector2 operator+(const iVector2& other) const {
            return iVector2(x + other.x, y + other.y);
        }

        iVector2 operator-(const iVector2& other) const {
            return iVector2(x - other.x, y - other.y);
        }
    };

    class iVector3 : public iVector2 {
    public:
        int z;

        iVector3(int x_ = 0, int y_ = 0, int z_ = 0) : iVector2(x_, y_), z(z_) {}

        bool operator==(const iVector3& other) const {
            return iVector2::operator==(other) && z == other.z;
        }

        bool operator!=(const iVector3& other) const {
            return !(*this == other);
        }

        iVector3 operator+(const iVector3& other) const {
            return iVector3(x + other.x, y + other.y, z + other.z);
        }

        iVector3 operator-(const iVector3& other) const {
            return iVector3(x - other.x, y - other.y, z - other.z);
        }
    };

    // Used for cell coordinates, which are always positive and small enough to fit in a short.
    class cellCoordinates : public iVector2 {
    public:
        cellCoordinates(int x_ = 0, int y_ = 0) : iVector2(x_, y_) {}
        
        // Conversion constructor from iVector2 (pixel coordinates to cell coordinates)
        explicit cellCoordinates(const iVector2& other) {
            // Transform the pixel based coordinates to the cell based, getting cell dimensions from the font manager
            int cellWidth = font::manager::getDefaultCellWidth();
            int cellHeight = font::manager::getDefaultCellHeight();
            
            // Avoid division by zero with reasonable fallbacks
            if (cellWidth <= 0) cellWidth = 8;   // Fallback to reasonable default
            if (cellHeight <= 0) cellHeight = 16; // Fallback to reasonable default
            
            x = other.x / cellWidth;
            y = other.y / cellHeight;
        }

        cellCoordinates& operator=(const iVector2& other) {
            // Transform the pixel based coordinates to the cell based, getting cell dimensions from the font manager
            int cellWidth = font::manager::getDefaultCellWidth();
            int cellHeight = font::manager::getDefaultCellHeight();
            
            // Avoid division by zero with reasonable fallbacks
            if (cellWidth <= 0) cellWidth = 8;   // Fallback to reasonable default
            if (cellHeight <= 0) cellHeight = 16; // Fallback to reasonable default
            
            x = other.x / cellWidth;
            y = other.y / cellHeight;
            
            return *this;
        }

        bool operator==(const cellCoordinates& other) const {
            return x == other.x && y == other.y;
        }

        bool operator!=(const cellCoordinates& other) const {
            return !(*this == other);
        }

        cellCoordinates operator+(const cellCoordinates& other) const {
            return cellCoordinates(x + other.x, y + other.y);
        }

        cellCoordinates operator-(const cellCoordinates& other) const {
            return cellCoordinates(x - other.x, y - other.y);
        }

        cellCoordinates operator*(const cellCoordinates& other) const {
            return cellCoordinates(x * other.x, y * other.y);
        }

        cellCoordinates operator/(const cellCoordinates& other) const {
            return cellCoordinates(x / other.x, y / other.y);
        }
    };

    struct rectangle {
        iVector3 position;
        iVector2 size;
    };

    struct RGB {
        uint8_t r, g, b;
    };

    struct Cell {
        char utf[4];    // Not null terminated since we already know it is 4 char long at max, leftovers are nulled out.

        RGB textColor;
        RGB backgroundColor;
    };

    inline uint32_t toXRGB8888(const RGB& color) {
        // Convert RGB to XRGB8888 format
        return (color.r << 16) | (color.g << 8) | color.b;
    }

    inline bool operator==(const RGB& a, const RGB& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }

    inline bool operator==(const Cell& a, const Cell& b) {
        return  a.textColor == b.textColor &&
                a.backgroundColor == b.backgroundColor && 
                ((uint64_t&)a.utf) == ((uint64_t&)b.utf); // Compare utf as a 64-bit integer for fast comparison
    }

    inline bool operator!=(const RGB& a, const RGB& b) {
        return !(a == b);
    }

    inline bool operator!=(const Cell& a, const Cell& b) {
        return !(a == b);
    }
}

#endif