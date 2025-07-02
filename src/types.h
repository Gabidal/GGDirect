#ifndef _TYPES_H_
#define _TYPES_H_

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

}

#endif