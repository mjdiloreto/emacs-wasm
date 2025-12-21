#!/bin/bash
# Build Emacs for WebAssembly using Emscripten
#
# This script automates the process of compiling Emacs to WASM.
# Prerequisites:
#   - Emscripten SDK installed and activated (source emsdk_env.sh)
#   - Standard build tools (autoconf, automake, texinfo)
#
# Usage:
#   ./build-wasm.sh [configure|build|clean|all]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wasm"
BUILD_NATIVE="${SCRIPT_DIR}/build-native"
EMACS_SRC="${SCRIPT_DIR}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if Emscripten is available
check_emscripten() {
    if ! command -v emcc &> /dev/null; then
        log_error "Emscripten not found. Please install and activate it:"
        echo ""
        echo "  # Install Emscripten SDK"
        echo "  git clone https://github.com/emscripten-core/emsdk.git"
        echo "  cd emsdk"
        echo "  ./emsdk install latest"
        echo "  ./emsdk activate latest"
        echo "  source ./emsdk_env.sh"
        echo ""
        exit 1
    fi

    log_info "Emscripten found: $(emcc --version | head -1)"
}

# Generate configure script if needed
run_autogen() {
    log_info "Running autogen.sh..."
    cd "$EMACS_SRC"

    if [ ! -f configure ]; then
        ./autogen.sh
    else
        log_info "configure already exists, skipping autogen"
    fi
}

# Build native helper tools (make-docfile, etc.)
# These must run on the host during the WASM build
build_native_tools() {
    log_info "Building native helper tools..."

    mkdir -p "$BUILD_NATIVE"
    cd "$BUILD_NATIVE"

    # Check if native tools already exist
    if [ -f "lib-src/make-docfile" ] && [ -f "lib-src/make-fingerprint" ]; then
        log_info "Native tools already built, skipping"
        return
    fi

    # Configure with native compiler (minimal config just for tools)
    log_info "Configuring native build for helper tools..."
    "$EMACS_SRC/configure" \
        --without-x \
        --without-ns \
        --without-gnutls \
        --without-xml2 \
        --without-json \
        --without-tree-sitter \
        --without-sqlite3 \
        --without-libgmp \
        --without-native-compilation \
        --without-modules \
        --disable-build-details

    # Build only the lib directory and lib-src tools
    log_info "Building native lib..."
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) -C lib

    log_info "Building native lib-src tools..."
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) -C lib-src

    log_info "Native helper tools built successfully"
}

# Build a stub library from wasm-stubs.c
# This is needed because configure links test programs against libraries
build_stub_library() {
    log_info "Building stub library for configure checks..."

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Compile wasm-stubs.c into an object file
    emcc -c "$EMACS_SRC/src/wasm-stubs.c" -o wasm-stubs.o -D__EMSCRIPTEN__ -I"$EMACS_SRC/src"

    # Create a static library
    emar rcs libtermcap.a wasm-stubs.o

    log_info "Created libtermcap.a with WASM stubs"
}

# Configure Emacs for WASM
run_configure() {
    log_info "Configuring Emacs for WASM build..."

    # Build native helper tools first (requires native compiler)
    build_native_tools

    # Build stub library for WASM
    build_stub_library

    cd "$BUILD_DIR"

    # Emscripten configure flags
    # Disable everything that requires impossible syscalls
    #
    # IMPORTANT: --host=wasm32-unknown-emscripten tells configure we're
    # cross-compiling for WASM, so it won't define DARWIN_OS, __FreeBSD__, etc.
    # This ensures the build is platform-agnostic regardless of the build machine.
    #
    # We pass -L. and -ltermcap to use our stub library
    emconfigure "$EMACS_SRC/configure" \
        --host=wasm32-unknown-emscripten \
        --build=$(cc -dumpmachine 2>/dev/null || echo "$(uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')") \
        --without-x \
        --without-ns \
        --without-w32 \
        --without-gnutls \
        --without-libotf \
        --without-m17n-flt \
        --without-xaw3d \
        --without-toolkit-scroll-bars \
        --without-xpm \
        --without-jpeg \
        --without-png \
        --without-gif \
        --without-tiff \
        --without-webp \
        --without-rsvg \
        --without-imagemagick \
        --without-xml2 \
        --without-json \
        --without-lcms2 \
        --without-libsystemd \
        --without-dbus \
        --without-gpm \
        --without-gsettings \
        --without-gconf \
        --without-selinux \
        --without-file-notification \
        --without-sound \
        --without-threads \
        --without-modules \
        --without-compress-install \
        --without-native-compilation \
        --without-libgmp \
        --without-tree-sitter \
        --without-sqlite3 \
        --with-pdumper=yes \
        --with-dumping=pdumper \
        --disable-build-details \
        CFLAGS="-O2 -g0" \
        LDFLAGS="-L$BUILD_DIR" \
        LIBS="-ltermcap" \
        LIBS_TERMCAP="-ltermcap" \
        ac_cv_have_decl_alarm=no \
        gl_cv_func_sleep_works=yes \
        ac_cv_func_nanosleep=yes \
        gl_cv_func_nanosleep=yes \
        ac_cv_func_malloc_trim=no \
        emacs_cv_ptys=no

    log_info "Configure complete. Build directory: $BUILD_DIR"
}

# Build Emacs with Emscripten
run_build() {
    log_info "Building Emacs for WASM..."

    cd "$BUILD_DIR"

    # Copy native helper tools to WASM build directory
    # These must be native binaries that can run on the host during build
    log_info "Copying native helper tools to WASM build..."
    mkdir -p lib-src
    for tool in make-docfile make-fingerprint etags ebrowse hexl movemail emacsclient; do
        if [ -f "$BUILD_NATIVE/lib-src/$tool" ]; then
            cp "$BUILD_NATIVE/lib-src/$tool" "lib-src/$tool"
            chmod +x "lib-src/$tool"
        fi
    done

    # Touch the tools with a future timestamp to prevent make from rebuilding them
    # This is crucial - otherwise emmake will rebuild them as WASM binaries
    touch -t $(date -v+1H +%Y%m%d%H%M.%S) lib-src/make-docfile lib-src/make-fingerprint 2>/dev/null || \
    touch -d "+1 hour" lib-src/make-docfile lib-src/make-fingerprint 2>/dev/null || \
    touch lib-src/make-docfile lib-src/make-fingerprint

    log_info "Native tools installed and timestamped"

    # Emscripten build flags
    # These are passed during the final link step
    EMSCRIPTEN_FLAGS=(
        "-s ASYNCIFY"
        "-s ASYNCIFY_STACK_SIZE=65536"
        "-s ALLOW_MEMORY_GROWTH=1"
        "-s INITIAL_MEMORY=268435456"      # 256MB
        "-s MAXIMUM_MEMORY=2147483648"      # 2GB
        "-s EXPORTED_RUNTIME_METHODS=['FS','callMain','cwrap','ccall']"
        "-s MODULARIZE=1"
        "-s EXPORT_NAME='createEmacs'"
        "-s FORCE_FILESYSTEM=1"
        "-s ENVIRONMENT='web,worker'"
        "-lidbfs.js"
        "-s NO_EXIT_RUNTIME=1"
        "-s ASSERTIONS=1"  # Enable for debugging, remove for production
    )

    # Join flags for LDFLAGS
    EMSCRIPTEN_LDFLAGS="${EMSCRIPTEN_FLAGS[*]}"

    log_info "Using Emscripten flags: $EMSCRIPTEN_LDFLAGS"

    # Build in stages to avoid rebuilding lib-src tools as WASM
    NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Stage 1: Build lib (gnulib) with Emscripten
    log_info "Stage 1: Building lib (gnulib)..."
    emmake make -j$NPROC -C lib

    # Stage 2: lib-src tools are already native, just verify they exist
    log_info "Stage 2: Verifying native lib-src tools..."
    if [ ! -x "lib-src/make-docfile" ]; then
        log_error "Native make-docfile not found!"
        exit 1
    fi

    # Stage 3: Build src (the actual Emacs binary)
    log_info "Stage 3: Building src..."
    # Include both the library path and Emscripten flags
    # Use /usr/bin/true as a no-op since native make-fingerprint can't modify WASM binaries
    emmake make -j$NPROC -C src \
        LDFLAGS="-L$BUILD_DIR $EMSCRIPTEN_LDFLAGS" \
        MAKE_PDUMPER_FINGERPRINT=/usr/bin/true

    log_info "Making WASM binary executable..."
    # Add Node.js shebang to make temacs directly executable
    if [ -f "src/temacs" ]; then
        # Use heredoc to avoid shell escaping issues with the shebang
        tail -n +2 src/temacs > src/temacs.body
        cat > src/temacs.new << 'SHEBANG'
#!/usr/bin/env node
SHEBANG
        cat src/temacs.body >> src/temacs.new
        mv src/temacs.new src/temacs
        rm src/temacs.body
        chmod +x src/temacs
        log_info "Added shebang to src/temacs"
    fi

    log_info "Build complete!"

    # Check for output files
    if [ -f "src/emacs.js" ]; then
        log_info "Generated files:"
        ls -la src/emacs.*
    else
        log_warn "emacs.js not found - build may have failed at link stage"
    fi
}

# Clean build directory
run_clean() {
    log_info "Cleaning build directories..."

    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        log_info "Removed $BUILD_DIR"
    else
        log_info "WASM build directory doesn't exist"
    fi
}

# Clean everything including native tools
run_clean_all() {
    run_clean

    if [ -d "$BUILD_NATIVE" ]; then
        rm -rf "$BUILD_NATIVE"
        log_info "Removed $BUILD_NATIVE"
    else
        log_info "Native build directory doesn't exist"
    fi
}

# Create a minimal HTML test harness
create_test_harness() {
    log_info "Creating test harness..."

    cat > "$BUILD_DIR/index.html" << 'EOF'
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Emacs WASM</title>
    <style>
        body {
            font-family: monospace;
            background: #1e1e1e;
            color: #d4d4d4;
            margin: 0;
            padding: 20px;
        }
        #output {
            white-space: pre;
            font-size: 14px;
            line-height: 1.4;
        }
        #status {
            color: #4ec9b0;
            margin-bottom: 10px;
        }
        .error { color: #f44747; }
        .warn { color: #ce9178; }
    </style>
</head>
<body>
    <div id="status">Loading Emacs WASM...</div>
    <div id="output"></div>

    <script>
        const output = document.getElementById('output');
        const status = document.getElementById('status');

        function log(msg, type = '') {
            const line = document.createElement('div');
            line.textContent = msg;
            if (type) line.className = type;
            output.appendChild(line);
            output.scrollTop = output.scrollHeight;
        }

        // Override console for debugging
        const originalConsole = { ...console };
        console.log = (...args) => {
            originalConsole.log(...args);
            log(args.join(' '));
        };
        console.error = (...args) => {
            originalConsole.error(...args);
            log(args.join(' '), 'error');
        };
        console.warn = (...args) => {
            originalConsole.warn(...args);
            log(args.join(' '), 'warn');
        };

        // Load Emacs module
        async function loadEmacs() {
            try {
                status.textContent = 'Importing Emacs module...';
                const createEmacs = (await import('./src/emacs.js')).default;

                status.textContent = 'Initializing Emacs...';
                const Module = await createEmacs({
                    print: (text) => log(text),
                    printErr: (text) => log(text, 'error'),
                    onRuntimeInitialized: () => {
                        status.textContent = 'Emacs runtime initialized!';
                    }
                });

                status.textContent = 'Emacs loaded!';

                // Expose Module globally for debugging
                window.EmacsModule = Module;

                // Try to start Emacs
                log('Starting Emacs...');
                Module.callMain(['-nw', '--batch', '--eval', '(print emacs-version)']);

            } catch (err) {
                status.textContent = 'Failed to load Emacs';
                status.className = 'error';
                log('Error: ' + err.message, 'error');
                log(err.stack, 'error');
            }
        }

        loadEmacs();
    </script>
</body>
</html>
EOF

    log_info "Test harness created at $BUILD_DIR/index.html"
    log_info "Serve with: python3 -m http.server 8080 --directory $BUILD_DIR"
    log_info "Note: Must use Cross-Origin headers for SharedArrayBuffer:"
    log_info "  Access-Control-Allow-Origin: *"
    log_info "  Cross-Origin-Opener-Policy: same-origin"
    log_info "  Cross-Origin-Embedder-Policy: require-corp"
}

# Show usage
show_usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  configure  - Build native tools, then configure for WASM"
    echo "  build      - Build Emacs WASM (requires configure first)"
    echo "  clean      - Remove WASM build directory (keeps native tools)"
    echo "  clean-all  - Remove all build directories"
    echo "  all        - Run configure + build"
    echo "  harness    - Create HTML test harness"
    echo "  help       - Show this help"
    echo ""
    echo "Example workflow:"
    echo "  $0 configure"
    echo "  $0 build"
    echo "  $0 harness"
}

# Main
case "${1:-help}" in
    configure)
        check_emscripten
        run_autogen
        run_configure
        ;;
    build)
        check_emscripten
        run_build
        ;;
    clean)
        run_clean
        ;;
    clean-all)
        run_clean_all
        ;;
    all)
        check_emscripten
        run_autogen
        run_configure
        run_build
        create_test_harness
        ;;
    harness)
        create_test_harness
        ;;
    help|--help|-h)
        show_usage
        ;;
    *)
        log_error "Unknown command: $1"
        show_usage
        exit 1
        ;;
esac
