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
BUILD_NATIVE="${SCRIPT_DIR}/build-native"
EMACS_SRC="${SCRIPT_DIR}"
# A checking build may opt into a separate directory.  The normal path is
# unchanged, so this cannot overwrite the shipping artifacts accidentally.
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build-wasm}"

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

    # Regenerate if configure is missing OR stale relative to its inputs
    # (configure.ac, any *.m4 macro). Without this check, a `git pull`/
    # rebase that touches configure.ac silently left a stale `configure`
    # in place, causing hard-to-diagnose mismatches (e.g. a renamed input
    # file that only the newly-regenerated configure knows about).
    if [ ! -f configure ] || [ configure.ac -nt configure ] || \
       find m4 -name '*.m4' -newer configure 2>/dev/null | grep -q .; then
        ./autogen.sh
    else
        log_info "configure is up to date, skipping autogen"
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

    # The terminfo %-language interpreter is a separate file so the host
    # cc can compile it for native unit tests (emacs-build-wasm
    # scripts/test/test-tparm.c).
    emcc -c "$EMACS_SRC/src/wasm-tparm.c" -o wasm-tparm.o -D__EMSCRIPTEN__ -I"$EMACS_SRC/src"

    # Create a static library
    emar rcs libtermcap.a wasm-stubs.o wasm-tparm.o

    log_info "Created libtermcap.a with WASM stubs"
}

# Configure Emacs for WASM
run_configure() {
    log_info "Configuring Emacs for WASM build..."

    # Build native helper tools first (requires native compiler)
    build_native_tools

    # Build stub library for WASM
    build_stub_library

    CHECKING_FLAGS=()
    if [ "${CHECKING_BUILD:-}" = "yes" ]; then
        # Keep the runtime consistency checks and the string guards that
        # amplify the corruption this gate is intended to expose.  This is
        # deliberately opt-in; the shipping build remains fast.
        CHECKING_FLAGS+=(--enable-checking=yes,stringbytes,stringoverrun,stringfreelist)
        log_info "Configuring separate GC checking build"
    fi

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
        "${CHECKING_FLAGS[@]}" \
        CFLAGS="-O2 -g -pthread" \
        LDFLAGS="-L$BUILD_DIR" \
        LIBS="-ltermcap" \
        LIBS_TERMCAP="-ltermcap" \
        gl_cv_func_sleep_works=yes \
        ac_cv_func_nanosleep=yes \
        gl_cv_func_nanosleep=yes \
        ac_cv_func_malloc_trim=no \
        emacs_cv_ptys=no \
        emacs_cv_usable_SIGIO=no

    log_info "Configure complete. Build directory: $BUILD_DIR"
}

# Build Emacs with Emscripten
run_build() {
    log_info "Building Emacs for WASM..."

    cd "$BUILD_DIR"

    # Clean polluted source tree files that interfere with #include_next
    # These are generated headers that should only exist in build directories
    log_info "Checking for polluted source tree files..."
    POLLUTED_FILES=(
        "$EMACS_SRC/lib/sys/stat.h"
        "$EMACS_SRC/lib/sys/random.h"
        "$EMACS_SRC/lib/sys/select.h"
        "$EMACS_SRC/lib/sys/time.h"
        "$EMACS_SRC/lib/sys/types.h"
    )
    for polluted_file in "${POLLUTED_FILES[@]}"; do
        if [ -f "$polluted_file" ]; then
            log_warn "Removing polluted file: $polluted_file"
            rm -f "$polluted_file"
        fi
    done

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

    # Preserve native libgnu.a for use by native tools
    log_info "Preserving native libgnu.a..."
    if [ -f "$BUILD_NATIVE/lib/libgnu.a" ]; then
        cp "$BUILD_NATIVE/lib/libgnu.a" "lib/libgnu-native.a"
        log_info "  Native libgnu.a saved as lib/libgnu-native.a"
    else
        log_error "Native libgnu.a not found!"
        exit 1
    fi

    # Touch ALL native tools with future timestamp to prevent rebuilds
    log_info "Timestamping native tools to prevent rebuilds..."
    if date -v+1H +%Y%m%d%H%M.%S >/dev/null 2>&1; then
        # macOS - use -perm to find executables
        FUTURE_TIME=$(date -v+1H +%Y%m%d%H%M.%S)
        find lib-src -type f -perm +111 -exec touch -t "$FUTURE_TIME" {} \;
    elif date -d "+1 hour" +%Y%m%d%H%M.%S >/dev/null 2>&1; then
        # GNU date (Linux) - use -executable flag
        find lib-src -type f -executable -exec touch -d "+1 hour" {} \;
    else
        # Fallback: just touch all files in lib-src with current time
        find lib-src -type f -exec touch {} \;
    fi

    # Patch lib-src Makefile to use native libgnu.a
    log_info "Configuring lib-src to use native library..."
    if [ -f lib-src/Makefile ]; then
        sed -i.bak 's|LOADLIBES = \.\./lib/libgnu\.a|LOADLIBES = ../lib/libgnu-native.a|g' lib-src/Makefile

        if grep -q "libgnu-native.a" lib-src/Makefile; then
            log_info "  lib-src/Makefile patched successfully"
        else
            log_warn "  Makefile patch may have failed - check lib-src/Makefile"
        fi
    else
        log_error "lib-src/Makefile not found!"
        exit 1
    fi

    log_info "Native tools installed and timestamped"

    # Emacs's data/lisp install path is versioned (e.g.
    # /usr/local/share/emacs/32.0.50/lisp) and must match the version this
    # exact source tree reports at runtime, or Emacs can't find any of its
    # Lisp/charset files and refuses to start. Derive it from configure.ac
    # rather than hardcoding it, since upstream bumps this every dev cycle.
    EMACS_PKG_VERSION="$(sed -n "s/^AC_INIT(\[GNU Emacs\], \[\([^]]*\)\].*/\1/p" "$EMACS_SRC/configure.ac")"
    if [ -z "$EMACS_PKG_VERSION" ]; then
        log_error "Could not determine Emacs version from configure.ac"
        exit 1
    fi
    log_info "Emacs version: $EMACS_PKG_VERSION"

    # Emscripten build flags
    # These are passed during the final link step
    EMSCRIPTEN_FLAGS=(
        "-pthread"
        "-s PROXY_TO_PTHREAD"
        "-s STACK_SIZE=67108864"           # 64MB stack
        # Conservative-GC soundness. LLVM keeps non-address-taken pointers in
        # wasm locals, which mark_c_stack() cannot see; --spill-pointers
        # writes them into the scanned shadow stack, and --flatten exposes
        # operand-stack intermediates so it can. Do NOT remove: without these
        # the GC frees live objects (`##` symbols, "memory access out of
        # bounds"). See docs/plans/12-spilling-pointers.org; gate is
        # `make gc-stress`.
        #
        # HAZARD: this only covers Lisp_Object while it is i32. Configuring
        # --with-wide-int makes it i64, which --spill-pointers does not treat
        # as a pointer — the fix would silently stop rooting Lisp values with
        # no build error. scripts/test/memory-budget.mjs fails if that flag
        # appears here.
        "-s BINARYEN_EXTRA_PASSES=--flatten,--spill-pointers"
        "-s ALLOW_MEMORY_GROWTH=1"
        "-s INITIAL_MEMORY=268435456"      # 256MB
        "-s MAXIMUM_MEMORY=2147483648"      # 2GB
        "-s EXPORTED_RUNTIME_METHODS=['FS','NODEFS','callMain','cwrap','ccall','ENV','wasmMemory']"
        "-s MODULARIZE=1"
        "-s EXPORT_NAME='createEmacs'"
        "-s FORCE_FILESYSTEM=1"
        "-s ENVIRONMENT='web,worker,node'"
        "-lidbfs.js"
        "-lnodefs.js"
        "-s NO_EXIT_RUNTIME=1"
        "-s ASSERTIONS=2"
        # Must stay at 1, not 2. Level 2 injects calls into Emscripten's
        # absolute stack primitives; --spill-pointers then frames those and
        # restores the stack pointer to a stale value, so boot underflows.
        # Level 1 keeps the JS stack cookie without that instrumentation.
        "-s STACK_OVERFLOW_CHECK=1"

        # xterm-pty: connects Emacs TTY I/O to xterm.js via PTY layer
        "--js-library" "${EMACS_SRC}/../web/node_modules/xterm-pty/emscripten-pty.js"

        # Bundle Emacs data files into virtual filesystem
        # These are required for Emacs to start (lisp files, charsets, etc.)
        "--preload-file" "${EMACS_SRC}/lisp@/usr/local/share/emacs/${EMACS_PKG_VERSION}/lisp"
        "--preload-file" "${EMACS_SRC}/etc@/usr/local/share/emacs/${EMACS_PKG_VERSION}/etc"

        # The frontends (doom-index.html, dist-index.html) inject their own
        # subdirs.el into the packaged lisp dir during preRun, and the
        # package loader throws EEXIST if the archive then delivers one.
        # A native in-tree build (e.g. prepare-doom-home.sh's doom sync)
        # drops a generated subdirs.el into ${EMACS_SRC}/lisp, so exclude
        # it here to keep the package deterministic either way.
        "--exclude-file" "*subdirs.el"
    )

    # Join flags for LDFLAGS
    EMSCRIPTEN_LDFLAGS="${EMSCRIPTEN_FLAGS[*]}"

    log_info "Using Emscripten flags: $EMSCRIPTEN_LDFLAGS"

    # Build in stages to avoid rebuilding lib-src tools as WASM
    NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Stage 1: Build lib (gnulib) with Emscripten
    log_info "Stage 1: Building lib (gnulib)..."
    # Force rebuild of lib if needed
    if [ ! -f "lib/libgnu.a" ] || [ $(find lib -name "*.o" | wc -l) -lt 80 ]; then
        log_info "  Cleaning lib directory for full rebuild..."
        emmake make -C lib clean 2>/dev/null || true
    fi
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
    # Use -o flag to tell make these native tools are already up-to-date

    # Build just temacs first (not 'all' which triggers bootstrap)
    log_info "Stage 3a: Building temacs..."
    emmake make -j1 -C src temacs \
        LDFLAGS="-L$BUILD_DIR $EMSCRIPTEN_LDFLAGS" \
        MAKE_PDUMPER_FINGERPRINT=/usr/bin/true

    # Add shebang to make temacs executable under Node.js
    # This MUST happen before any step that tries to run temacs
    log_info "Making WASM binary executable..."
    if [ -f "src/temacs" ]; then
        # Prepend shebang without removing any existing code
        echo '#!/usr/bin/env node' | cat - src/temacs > src/temacs.new
        mv src/temacs.new src/temacs
        chmod +x src/temacs
        log_info "Added shebang to src/temacs"

        tail -n +2 src/temacs > src/temacs.js
        log_info "temacs copied to js file for browser testing"
    else
        log_error "temacs not found - build failed!"
        exit 1
    fi

    # Create pdumper bootstrap by running temacs under Node.js
    log_info "Stage 3b: Creating bootstrap-emacs.pdmp via Node.js..."
    PDUMP_OUTPUT="${BUILD_DIR}/src/bootstrap-emacs.pdmp"
    WASM_BUILD_DIR="$BUILD_DIR" node --stack-size=65536 \
        "${SCRIPT_DIR}/../emacs-wasm.mjs" bootstrap-dump \
        "${PDUMP_OUTPUT}" && {
        log_info "bootstrap-emacs.pdmp created: $(ls -lh "${PDUMP_OUTPUT}" | awk '{print $5}')"
    } || {
        log_warn "pdump creation failed - startup will use loadup.el (~30s)"
        log_warn "Re-run: node --stack-size=65536 emacs-wasm.mjs bootstrap-dump"
    }

    log_info "Build complete!"

    # Check for output files
    if [ -f "src/temacs" ]; then
        log_info "Generated files:"
        ls -la src/temacs* src/*.wasm 2>/dev/null || true
    else
        log_warn "temacs not found - build may have failed at link stage"
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

    # Also clean any stale .o files from source tree
    # These can accumulate from failed builds and confuse make
    log_info "Cleaning stale object files from source tree..."
    find "$EMACS_SRC/src" -name "*.o" -delete 2>/dev/null || true
    find "$EMACS_SRC/lib" -name "*.o" -delete 2>/dev/null || true
    log_info "Source tree cleaned"
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
        # Refresh the stub library (D7): it is otherwise only built at
        # configure time, so a `build` after editing wasm-stubs.c or
        # wasm-tparm.c would silently link the stale libtermcap.a.
        build_stub_library
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
        run_clean      # Ensure fresh build with no stale files
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
