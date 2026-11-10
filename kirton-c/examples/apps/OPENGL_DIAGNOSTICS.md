# OpenGL diagnostics (Windows)

These examples use only Kirton's public `gl` module. Run them from the `kirton-c` directory so the relative script paths resolve:

```powershell
.\build-ninja\kirton.exe .\examples\apps\gl_test_a_compat.kir
.\build-ninja\kirton.exe .\examples\apps\gl_test_b_core.kir
.\build-ninja\kirton.exe .\examples\apps\gl_test_c_moving.kir
.\build-ninja\kirton.exe .\examples\apps\gl_test_d_pong.kir
.\build-ninja\kirton.exe .\examples\apps\gl_test_context_lifecycle.kir
```

Close each interactive demo window to exit. A, D use the compatibility profile; B and C explicitly request OpenGL 3.3 Core. The lifecycle test creates both profiles, checks that invalid GLSL is rejected, allocates and releases GPU objects, destroys the context, and recreates the window.

The runtime writes vendor, renderer, OpenGL version, GLSL version, and selected profile to stderr at window creation. WGL failures, shader compile/link logs, SwapBuffers failures, and OpenGL errors are also written to stderr. For a saved diagnostic log:

```powershell
.\build-ninja\kirton.exe .\examples\apps\gl_test_b_core.kir 2> .\gl-core.log
```

On the development Windows machine, the runtime reported NVIDIA GeForce RTX 5070 Ti, OpenGL 3.3.0 Core, profile mask `0x1`, and GLSL 3.30 for the core demo; the compatibility demo reported OpenGL 4.6.0. The GL lifecycle test printed `GL_CONTEXT_LIFECYCLE_OK`. Visual appearance still needs direct human inspection because this session's desktop capture did not expose the Kirton window.
