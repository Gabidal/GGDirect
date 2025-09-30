#ifndef _RENDERER_H_
#define _RENDERER_H_

#include "window.h"
#include "display.h"
#include "font.h"

namespace renderer {
    extern void init();    // Sets up the rendering thread, which polls data from the handles and transform them into a renderable format for DRM.
    extern void exit();
}

#endif