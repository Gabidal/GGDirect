#ifndef _WINDOW_H_
#define _WINDOW_H_

#include "types.h"
#include "tcp.h"
#include "guard.h"

#include <vector>


namespace window {
    /*
    As each GGUI gets its input from the terminal hosting it. We currently need to first instate a new terminal and then host GGUI on top of it, for GGUI to get input from it.
    We can later on, give each handle Focused mode, and perpetrate the inputs from here and give them through sockets to each individual GGUI instance. 
    */
    class handle {
    public:
        // I'm not sure where we can get the position of hosting terminal for the current GGUI handle, this will be more important once we start to give inputs from here and ditch terminal hosting.
        types::iVector3 position;   // Z represents draw order, higher = later draw.
        types::iVector2 size;       // Updated two directionally, from GGUI to here via terminal size update, or from here to GGUI when we ditch terminal hosting.
    
        // This is the final and upholding socket that is formed after the handshake and reroute.
        tcp::connection connection;

        handle(tcp::connection&& conn) : position({}), size({}), connection(std::move(conn)){}

        void close() {
            connection.close();
        }
    };

    // Manages all of the handles and their handshake protocol steps.
    namespace manager {
        extern atomic::guard<std::vector<handle>> handles;
        
        // First we will make an listener with port number zero, to invoke the kernel giving us an empty port number to use.
        extern atomic::guard<tcp::listener> listener;

        extern void init();

        extern void close();
    };
}

#endif