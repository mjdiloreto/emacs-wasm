# Building Emacs for WebAssembly

This document describes how to compile Emacs to WebAssembly (WASM) for running in web browsers.

## Prerequisites

### 1. Install Emscripten SDK

```bash
# Clone the SDK
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Download and install the latest SDK
./emsdk install latest
./emsdk activate latest

# Activate PATH and environment variables
source ./emsdk_env.sh

# Verify installation
emcc --version
```

### 2. Standard Build Tools

On macOS:
```bash
brew install autoconf automake texinfo gnutls
```

On Ubuntu/Debian:
```bash
sudo apt install autoconf automake texinfo libgnutls28-dev
```

## Building

### Quick Start

```bash
# One-command build
./build-wasm.sh all
```

### Step by Step

```bash
# 1. Generate configure script (first time only)
./autogen.sh

# 2. Configure for WASM
./build-wasm.sh configure

# 3. Build
./build-wasm.sh build

# 4. Create test harness
./build-wasm.sh harness
```

### Build Output

After a successful build, you'll find:
- `build-wasm/src/emacs.js` - JavaScript loader/glue code
- `build-wasm/src/emacs.wasm` - WebAssembly binary
- `build-wasm/src/emacs.data` - Preloaded filesystem data
- `build-wasm/index.html` - Test harness

## Running in Browser

### Development Server with COOP/COEP Headers

For SharedArrayBuffer support (required for Atomics.wait), you need special headers:

```bash
# Using Python with a simple server (no SharedArrayBuffer)
python3 -m http.server 8080 --directory build-wasm

# Using a server with proper headers
npx serve build-wasm --cors --single

# Or use this Node.js server:
cat > serve.mjs << 'EOF'
import { createServer } from 'http';
import { readFile, stat } from 'fs/promises';
import { extname, join } from 'path';

const PORT = 8080;
const ROOT = './build-wasm';

const MIME_TYPES = {
    '.html': 'text/html',
    '.js': 'application/javascript',
    '.mjs': 'application/javascript',
    '.wasm': 'application/wasm',
    '.css': 'text/css',
    '.json': 'application/json',
    '.data': 'application/octet-stream',
};

createServer(async (req, res) => {
    // Required headers for SharedArrayBuffer
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Access-Control-Allow-Origin', '*');

    let path = join(ROOT, req.url === '/' ? '/index.html' : req.url);

    try {
        const data = await readFile(path);
        const ext = extname(path);
        res.setHeader('Content-Type', MIME_TYPES[ext] || 'application/octet-stream');
        res.end(data);
    } catch {
        res.statusCode = 404;
        res.end('Not found');
    }
}).listen(PORT, () => console.log(`Server: http://localhost:${PORT}`));
EOF

node serve.mjs
```

Then open http://localhost:8080 in your browser.

## Architecture Overview

```
Browser (Main Thread)           Browser (Web Worker)
┌─────────────────────┐         ┌─────────────────────┐
│ DOM Events          │         │ Emacs WASM Binary   │
│ Canvas Rendering    │ ◄─────► │ Lisp Interpreter    │
│ SharedArrayBuffer   │         │ Atomics.wait()      │
│ Atomics.notify()    │         │ Virtual Filesystem  │
└─────────────────────┘         └─────────────────────┘
                                          │
                                    WebSocket
                                          │
                                          ▼
                               ┌─────────────────────┐
                               │ Remote Backend      │
                               │ (Process Execution) │
                               │ (Real Filesystem)   │
                               └─────────────────────┘
```

## Key Implementation Details

### WASM Stubs (`src/wasm-stubs.c`)

This file provides stub implementations for system calls impossible in WASM:

- **Process management**: `fork`, `exec`, `wait`, `kill` - Return ENOSYS
- **Signal handling**: `sigprocmask`, `sigaction` - Track state but limited delivery
- **TTY operations**: `ioctl`, `tcgetattr`, `tcsetattr` - Return defaults
- **User/group**: `getuid`, `getgid` - Return fake values (1000)

### Build Configuration

The build disables:
- X11, NS (macOS), W32 (Windows) window systems
- Image libraries (PNG, JPEG, GIF, etc.)
- TLS/networking libraries
- D-Bus, systemd integration
- Sound, file notifications
- Native compilation, modules

It enables:
- Portable dumper (pdumper) for fast startup
- Basic terminal mode operation

### Emscripten Flags

| Flag | Purpose |
|------|---------|
| `ASYNCIFY` | Allow blocking calls to yield |
| `ASYNCIFY_STACK_SIZE=65536` | Stack for async unwinding |
| `ALLOW_MEMORY_GROWTH` | Dynamic heap allocation |
| `INITIAL_MEMORY=256MB` | Starting heap size |
| `MAXIMUM_MEMORY=2GB` | Heap limit |
| `FORCE_FILESYSTEM` | Enable virtual FS |
| `MODULARIZE` | Export as ES6 module |
| `ENVIRONMENT=web,worker` | Target environments |

## Known Limitations

1. **No Subprocesses**: `M-x shell`, `M-x compile`, `async-shell-command` won't work without remote backend
2. **No Real Filesystem**: Uses Emscripten virtual FS (MEMFS/IDBFS)
3. **Limited Signals**: `C-g` works but process signals don't
4. **Memory**: Large files may exhaust WASM memory
5. **Performance**: Asyncify adds ~30% overhead

## Remote Backend (Future Work)

For full functionality, connect to a remote backend that provides:
- Process execution (PTY for shells, pipes for commands)
- Real filesystem access
- File watching (inotify)
- Network proxying

The backend communicates via WebSocket with a simple JSON protocol:

```json
// Start process
{"type": "process.create", "id": "1", "cmd": ["grep", "-r", "TODO", "."]}

// Process output
{"type": "process.output", "id": "1", "stream": "stdout", "data": "..."}

// Process exit
{"type": "process.exit", "id": "1", "code": 0}
```

## Troubleshooting

### Build Errors

**"undefined reference to fork"**
- Ensure `src/wasm-stubs.c` is included in build
- Add to `src/Makefile.in`: `WASM_OBJ = wasm-stubs.o`

**"memory access out of bounds"**
- Increase `INITIAL_MEMORY` or `MAXIMUM_MEMORY`
- Check for infinite loops in Lisp code

**"SharedArrayBuffer is not defined"**
- Serve with COOP/COEP headers (see above)
- Use Chrome or Firefox with flags enabled

### Runtime Errors

**"Cannot find module X"**
- Bundle required Lisp files in WASM data
- Check `--preload-file` in build

**"Maximum call stack exceeded"**
- Increase `ASYNCIFY_STACK_SIZE`
- Reduce recursion depth in Elisp

## References

- [vim.wasm](https://github.com/rhysd/vim.wasm) - Similar project for Vim
- [Emscripten Documentation](https://emscripten.org/docs/)
- [WASI](https://wasi.dev/) - WebAssembly System Interface
- [xterm.js](https://xtermjs.org/) - Terminal emulator for browser

## Contributing

1. Test with `--batch` mode first
2. Add Asyncify annotations for blocking functions
3. Consider which Elisp packages need filesystem access
4. Profile memory usage with browser devtools
