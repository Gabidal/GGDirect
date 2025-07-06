#ifndef _INPUT_H_
#define _INPUT_H_

#include "types.h"
#include "tcp.h"

namespace input {

    void init();  // Initializes input thread and sets up necessary resources
    void exit();  // Cleans up input resources and exits the input thread

    void poll(); // Called from input thread to poll for input events and to then send them to the focused handle

}

#endif