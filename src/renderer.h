#ifndef _RENDERER_H_
#define _RENDERER_H_

#include "window.h"
#include "display.h"
#include "font.h"

namespace renderer {
    extern void init();    // Sets up the rendering thread, which polls data from the handles and transform them into a renderable format for DRM.
    extern bool renderHandle(const window::handle* handle);  // Returns true if rendering occurred
    extern void exit();
    
    // Helper function to render cell data to framebuffer
    extern void renderCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight,
                                       int startX, int startY, const font::cellRenderData& cellData);
}

#endif