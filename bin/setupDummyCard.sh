#!/bin/bash

#===============================================================================
# GGDirect - Dummy DRM Card Setup Script
#===============================================================================
# Description: Professional script to set up dummy DRM devices for development
#              environments that lack hardware GPU access (like WSL2, containers)
# Author:      GGDirect Development Team
# Version:     1.0.0
# License:     MIT
#===============================================================================

set -euo pipefail  # Exit on error, undefined vars, pipe failures

#===============================================================================
# CONFIGURATION
#===============================================================================

readonly SCRIPT_NAME="$(basename "$0")"
readonly SCRIPT_VERSION="1.0.0"
readonly LOG_FILE="/tmp/ggdirect_setup.log"
readonly DRI_DIR="/dev/dri"
readonly BACKUP_DIR="/tmp/ggdirect_backup"

# DRM device configurations (major=226 for DRM)
declare -A DRM_DEVICES=(
    ["card0"]="226,0"
    ["card1"]="226,1"
    ["renderD128"]="226,128"
    ["renderD129"]="226,129"
    ["controlD64"]="226,64"
    ["controlD65"]="226,65"
)

# Package dependencies
readonly PACKAGES=(
    "mesa-utils"
    "libdrm2"
    "libdrm-dev"
    "build-essential"
    "libgl1-mesa-dev"
    "libgl1-mesa-glx"
    "libglapi-mesa"
    "libgles2-mesa-dev"
    "libegl1-mesa-dev"
)

# Colors for output
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'
readonly PURPLE='\033[0;35m'
readonly CYAN='\033[0;36m'
readonly NC='\033[0m' # No Color

#===============================================================================
# LOGGING AND OUTPUT FUNCTIONS
#===============================================================================

log() {
    local level="$1"
    shift
    local message="$*"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$timestamp] [$level] $message" >> "$LOG_FILE"
}

info() {
    echo -e "${BLUE}[INFO]${NC} $*"
    log "INFO" "$*"
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $*"
    log "SUCCESS" "$*"
}

warning() {
    echo -e "${YELLOW}[WARNING]${NC} $*"
    log "WARNING" "$*"
}

error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
    log "ERROR" "$*"
}

debug() {
    if [[ "${DEBUG:-0}" == "1" ]]; then
        echo -e "${PURPLE}[DEBUG]${NC} $*"
        log "DEBUG" "$*"
    fi
}

#===============================================================================
# UTILITY FUNCTIONS
#===============================================================================

print_header() {
    echo -e "${CYAN}"
    echo "==============================================================================="
    echo " GGDirect - Dummy DRM Card Setup Script v${SCRIPT_VERSION}"
    echo "==============================================================================="
    echo -e "${NC}"
}

print_usage() {
    cat << EOF
Usage: $SCRIPT_NAME [OPTIONS]

Set up dummy DRM devices for development environments lacking GPU hardware.

OPTIONS:
    -h, --help              Show this help message
    -v, --verbose           Enable verbose output
    -d, --debug             Enable debug mode
    -c, --cleanup           Remove existing dummy devices and cleanup
    -b, --backup            Create backup of existing devices
    -r, --restore           Restore from backup
    -l, --list              List current DRM devices
    -t, --test              Test DRM functionality after setup
    -f, --force             Force setup even if devices exist
    -q, --quiet             Suppress non-error output
    --version               Show version information

EXAMPLES:
    $SCRIPT_NAME                    # Standard setup
    $SCRIPT_NAME -v -t             # Verbose setup with testing
    $SCRIPT_NAME -c                # Cleanup existing setup
    $SCRIPT_NAME -b                # Create backup before setup
    $SCRIPT_NAME -r                # Restore from backup

EOF
}

check_requirements() {
    debug "Checking system requirements..."
    
    # Check if running as root or with sudo
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root or with sudo privileges"
        exit 1
    fi
    
    # Check if running in supported environment
    if [[ ! -f /proc/version ]]; then
        error "Unable to determine system type"
        exit 1
    fi
    
    local kernel_info=$(cat /proc/version)
    if [[ "$kernel_info" =~ WSL ]]; then
        info "Detected WSL environment"
    elif [[ "$kernel_info" =~ container ]]; then
        info "Detected container environment"
    else
        warning "Unknown environment - proceeding with caution"
    fi
    
    # Check for required commands
    local required_commands=("mknod" "chmod" "chown" "mkdir" "apt-get")
    for cmd in "${required_commands[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            error "Required command '$cmd' not found"
            exit 1
        fi
    done
    
    success "System requirements check passed"
}

is_device_node() {
    local device="$1"
    [[ -c "$device" ]] || [[ -b "$device" ]]
}

backup_existing_devices() {
    info "Creating backup of existing DRM devices..."
    
    mkdir -p "$BACKUP_DIR"
    
    if [[ -d "$DRI_DIR" ]]; then
        cp -r "$DRI_DIR" "$BACKUP_DIR/dri_backup_$(date +%Y%m%d_%H%M%S)" 2>/dev/null || true
        success "Backup created in $BACKUP_DIR"
    else
        info "No existing DRM devices to backup"
    fi
}

restore_from_backup() {
    info "Restoring DRM devices from backup..."
    
    local latest_backup=$(ls -1t "$BACKUP_DIR"/dri_backup_* 2>/dev/null | head -1)
    
    if [[ -z "$latest_backup" ]]; then
        error "No backup found in $BACKUP_DIR"
        exit 1
    fi
    
    # Remove current devices
    rm -rf "$DRI_DIR" 2>/dev/null || true
    
    # Restore from backup
    cp -r "$latest_backup" "$DRI_DIR"
    
    success "Restored DRM devices from backup: $(basename "$latest_backup")"
}

#===============================================================================
# MAIN FUNCTIONALITY
#===============================================================================

install_packages() {
    info "Installing required packages..."
    
    # Update package list
    apt-get update -qq || {
        error "Failed to update package list"
        exit 1
    }
    
    # Install packages
    local failed_packages=()
    for package in "${PACKAGES[@]}"; do
        debug "Installing package: $package"
        if apt-get install -y "$package" &>> "$LOG_FILE"; then
            success "Installed: $package"
        else
            warning "Failed to install: $package"
            failed_packages+=("$package")
        fi
    done
    
    if [[ ${#failed_packages[@]} -gt 0 ]]; then
        warning "Some packages failed to install: ${failed_packages[*]}"
        warning "Continuing with setup..."
    fi
}

create_dri_directory() {
    info "Creating DRI directory structure..."
    
    if [[ -d "$DRI_DIR" && "$FORCE_SETUP" != "1" ]]; then
        warning "DRI directory already exists: $DRI_DIR"
        return 0
    fi
    
    mkdir -p "$DRI_DIR"
    chmod 755 "$DRI_DIR"
    
    success "Created DRI directory: $DRI_DIR"
}

create_device_nodes() {
    info "Creating DRM device nodes..."
    
    local created_devices=()
    local failed_devices=()
    
    for device in "${!DRM_DEVICES[@]}"; do
        local device_path="$DRI_DIR/$device"
        local major_minor="${DRM_DEVICES[$device]}"
        local major="${major_minor%,*}"
        local minor="${major_minor#*,}"
        
        debug "Creating device: $device_path (major=$major, minor=$minor)"
        
        # Skip if device exists and not forcing
        if [[ -e "$device_path" && "$FORCE_SETUP" != "1" ]]; then
            warning "Device already exists: $device_path"
            continue
        fi
        
        # Remove existing device if forcing
        if [[ -e "$device_path" ]]; then
            rm -f "$device_path"
        fi
        
        # Create device node
        if mknod "$device_path" c "$major" "$minor" 2>> "$LOG_FILE"; then
            chmod 666 "$device_path"
            chown root:root "$device_path"
            created_devices+=("$device")
            success "Created device: $device"
        else
            failed_devices+=("$device")
            error "Failed to create device: $device"
        fi
    done
    
    if [[ ${#created_devices[@]} -gt 0 ]]; then
        success "Successfully created ${#created_devices[@]} device nodes"
    fi
    
    if [[ ${#failed_devices[@]} -gt 0 ]]; then
        warning "Failed to create ${#failed_devices[@]} device nodes"
    fi
}

list_drm_devices() {
    echo -e "${CYAN}Current DRM devices:${NC}"
    
    if [[ ! -d "$DRI_DIR" ]]; then
        echo "No DRI directory found"
        return 1
    fi
    
    ls -la "$DRI_DIR" | while read -r line; do
        if [[ "$line" =~ ^c ]]; then
            echo -e "${GREEN}$line${NC}"
        elif [[ "$line" =~ ^d ]]; then
            echo -e "${BLUE}$line${NC}"
        else
            echo "$line"
        fi
    done
}

test_drm_functionality() {
    info "Testing DRM functionality..."
    
    local test_results=()
    
    # Test device accessibility
    for device in "${!DRM_DEVICES[@]}"; do
        local device_path="$DRI_DIR/$device"
        if [[ -c "$device_path" ]]; then
            if [[ -r "$device_path" && -w "$device_path" ]]; then
                test_results+=("✓ $device: accessible")
            else
                test_results+=("✗ $device: permission denied")
            fi
        else
            test_results+=("✗ $device: not found")
        fi
    done
    
    # Test with drm utilities if available
    if command -v modetest &> /dev/null; then
        debug "Running modetest..."
        if modetest -c &>> "$LOG_FILE"; then
            test_results+=("✓ modetest: passed")
        else
            test_results+=("✗ modetest: failed")
        fi
    fi
    
    # Test with glxinfo if available
    if command -v glxinfo &> /dev/null; then
        debug "Running glxinfo..."
        if glxinfo &>> "$LOG_FILE"; then
            test_results+=("✓ glxinfo: passed")
        else
            test_results+=("✗ glxinfo: failed (expected in WSL2)")
        fi
    fi
    
    echo -e "${CYAN}Test Results:${NC}"
    for result in "${test_results[@]}"; do
        if [[ "$result" =~ ✓ ]]; then
            echo -e "${GREEN}$result${NC}"
        else
            echo -e "${RED}$result${NC}"
        fi
    done
}

cleanup_devices() {
    info "Cleaning up dummy DRM devices..."
    
    if [[ -d "$DRI_DIR" ]]; then
        rm -rf "$DRI_DIR"
        success "Removed DRI directory: $DRI_DIR"
    else
        info "No DRI directory to clean up"
    fi
    
    # Clean up log file
    if [[ -f "$LOG_FILE" ]]; then
        rm -f "$LOG_FILE"
        success "Removed log file: $LOG_FILE"
    fi
}

setup_environment_variables() {
    info "Setting up environment variables..."
    
    local env_file="/etc/environment"
    local profile_file="/etc/profile.d/ggdirect-drm.sh"
    
    # Create profile script
    cat > "$profile_file" << 'EOF'
#!/bin/bash
# GGDirect DRM Environment Variables

export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export MESA_GL_VERSION_OVERRIDE=3.3
export DRI_PRIME=0

# Add DRM device path to common environment variables
export DRM_DEVICE_PATH="/dev/dri/card0"
export RENDER_DEVICE_PATH="/dev/dri/renderD128"
EOF
    
    chmod +x "$profile_file"
    success "Created environment profile: $profile_file"
}

#===============================================================================
# MAIN EXECUTION
#===============================================================================

main() {
    local CLEANUP=0
    local BACKUP=0
    local RESTORE=0
    local LIST_ONLY=0
    local TEST_ONLY=0
    local FORCE_SETUP=0
    local QUIET=0
    local VERBOSE=0
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                print_usage
                exit 0
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            -d|--debug)
                DEBUG=1
                shift
                ;;
            -c|--cleanup)
                CLEANUP=1
                shift
                ;;
            -b|--backup)
                BACKUP=1
                shift
                ;;
            -r|--restore)
                RESTORE=1
                shift
                ;;
            -l|--list)
                LIST_ONLY=1
                shift
                ;;
            -t|--test)
                TEST_ONLY=1
                shift
                ;;
            -f|--force)
                FORCE_SETUP=1
                shift
                ;;
            -q|--quiet)
                QUIET=1
                shift
                ;;
            --version)
                echo "GGDirect DRM Setup Script v${SCRIPT_VERSION}"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
    
    # Suppress output if quiet mode
    if [[ "$QUIET" == "1" ]]; then
        exec 1>/dev/null
    fi
    
    # Initialize log file
    echo "=== GGDirect DRM Setup Log - $(date) ===" > "$LOG_FILE"
    
    print_header
    
    # Handle special modes
    if [[ "$LIST_ONLY" == "1" ]]; then
        list_drm_devices
        exit 0
    fi
    
    if [[ "$RESTORE" == "1" ]]; then
        restore_from_backup
        exit 0
    fi
    
    if [[ "$CLEANUP" == "1" ]]; then
        cleanup_devices
        exit 0
    fi
    
    # Check requirements
    check_requirements
    
    # Create backup if requested
    if [[ "$BACKUP" == "1" ]]; then
        backup_existing_devices
    fi
    
    # Main setup process
    info "Starting DRM dummy device setup..."
    
    install_packages
    create_dri_directory
    create_device_nodes
    setup_environment_variables
    
    success "DRM dummy device setup completed successfully!"
    
    # Show device list
    list_drm_devices
    
    # Run tests if requested
    if [[ "$TEST_ONLY" == "1" ]]; then
        test_drm_functionality
    fi
    
    # Final instructions
    echo -e "\n${CYAN}Next Steps:${NC}"
    echo "1. Source the environment: source /etc/profile.d/ggdirect-drm.sh"
    echo "2. Test with: ls -la /dev/dri/"
    echo "3. Check log file: $LOG_FILE"
    echo -e "4. For your renderer, use: ${GREEN}/dev/dri/card0${NC}"
    
    success "Setup complete! Happy coding! 🚀"
}

# Execute main function with all arguments
main "$@"
