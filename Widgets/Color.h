#pragma once

#include <AK/Types.h>

typedef dword RGBA32;

class Color {
public:
    enum NamedColor {
        Black,
        White,
        Red,
        Green,
        Blue,
        DarkGray,
        MidGray,
        LightGray,
    };

    Color() { }
    Color(NamedColor);
    Color(byte r, byte g, byte b);

    RGBA32 value() const { return m_value; }

private:
    RGBA32 m_value { 0 };
};
