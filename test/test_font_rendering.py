#!/usr/bin/env python3
"""
Simple test client for GGDirect to demonstrate the font rendering system.
This script connects to the GGDirect server and sends sample terminal data.
"""

import socket
import struct
import time
import sys

def create_test_cell_buffer():
    """Create a test cell buffer with various characters and colors."""
    # Define some test data
    test_text = [
        "Hello, World! 🌍",
        "Font Test: ABC abc 123",
        "UTF-8: äöü αβγ 中文",
        "Symbols: ←→↑↓ ♪♫♬♭",
        "Box: ┌─┐│ │└─┘",
        "",
        "Colors and styles:",
        "Red text",
        "Green background",
        "Blue on yellow"
    ]
    
    width = 40
    height = len(test_text) + 2
    
    cells = []
    
    for y in range(height):
        for x in range(width):
            # Default cell
            utf8_chars = b'\x00\x00\x00\x00'  # Null character
            text_color = (255, 255, 255)      # White text
            bg_color = (0, 0, 0)              # Black background
            
            # Fill with test content
            if y > 0 and y <= len(test_text) and x < len(test_text[y-1]):
                char = test_text[y-1][x]
                utf8_chars = char.encode('utf-8')[:4].ljust(4, b'\x00')
                
                # Add some colors for demonstration
                if y == 7:  # "Red text"
                    text_color = (255, 0, 0)
                elif y == 8:  # "Green background"
                    bg_color = (0, 128, 0)
                elif y == 9:  # "Blue on yellow"
                    text_color = (0, 0, 255)
                    bg_color = (255, 255, 0)
            
            # Pack cell data: 4 bytes UTF-8 + 3 bytes text color + 3 bytes bg color
            cell_data = utf8_chars + struct.pack('BBB', *text_color) + struct.pack('BBB', *bg_color)
            cells.append(cell_data)
    
    return width, height, b''.join(cells)

def main():
    try:
        # Read the port from the gateway file
        try:
            with open('/tmp/GGDirect.gateway', 'r') as f:
                port = int(f.read().strip())
        except FileNotFoundError:
            print("GGDirect gateway file not found. Is GGDirect running?")
            return 1
        except ValueError:
            print("Invalid port in gateway file.")
            return 1
        
        print(f"Connecting to GGDirect on port {port}...")
        
        # Create a server socket first and get a dynamic port
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind(('localhost', 0))  # Let the OS assign a free port
        server_sock.listen(1)
        
        # Get the actual port that was assigned
        our_port = server_sock.getsockname()[1]
        print(f"Our server listening on port {our_port}")
        
        # Connect to GGDirect
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('localhost', port))
        
        # Send our port for the handshake
        sock.send(struct.pack('H', our_port))
        
        # Wait for confirmation and close initial connection
        sock.close()
        
        print("Waiting for GGDirect to connect back...")
        client_sock, addr = server_sock.accept()
        print(f"GGDirect connected from {addr}")
        
        # Receive confirmation
        confirmation = client_sock.recv(2)
        print(f"Received confirmation: {struct.unpack('H', confirmation)[0]}")
        
        # Create test cell buffer
        width, height, cell_data = create_test_cell_buffer()
        
        print(f"Sending test buffer: {width}x{height} ({len(cell_data)} bytes)")
        
        # Send buffer dimensions
        client_sock.send(struct.pack('ii', width, height))
        
        # Send cell buffer data
        client_sock.send(cell_data)
        
        print("Test data sent successfully!")
        print("The GGDirect font rendering system should now be processing the test data.")
        print("Check the GGDirect output for rendering information.")
        
        # Keep connection alive for a bit
        time.sleep(5)
        
        # Send a clean state (0,0) to indicate no more updates
        client_sock.send(struct.pack('ii', 0, 0))
        
        client_sock.close()
        server_sock.close()
        
        return 0
        
    except Exception as e:
        print(f"Error: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
