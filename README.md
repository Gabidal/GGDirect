# GGDirect
DRM backend for lib GGUI

## Steps:
 - Writes its own socket data into /tmp/GGDirect.gateway

 - ### From the GGUI side:
   - If GGDirect mode is enables, read /tmp/GGDirect.gateway file, and proceed to initiate a handshake.
 
 - ### In Connector manager thread
   - Waits for socket handshake, after receiving an handshake proposition and success, GGDirect will provide the GGUI client with the new TCP socket information and closes the connection with the GGUI. And opens the same TCP again waiting for more handshakes.
 
 - ### In Renderer thread
   - Reads from the handlers. Transforms the given UTF buffer into drawable raw pixel data for DRM

---
# Visualized handshake
```
┌────────────┐    Handshake     ┌────────────┐
│  GGUI App  │ ───────────────> │  DRM       │
│            │  Connect to      │  Gateway   │
│            │    :3000         │  Listener  │
└────────────┘ <─────────────── └────────────┘
                    |
                    |  assign port :4001
                    V
┌────────────┐   New Connection   ┌────────────┐
│  GGUI App  │ ─────────────────> │ DRM per-   │
│            │     to :4001       │ client conn│
└────────────┘                    └────────────┘
```