# URUSHI — notes for AI assistants

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): component/file map, thread model, lock order, Lua boundary rules,
Safe mode / crash handling, build instructions and gotchas. Script security model: [docs/SECURITY.md](docs/SECURITY.md). Cloud library: [docs/CLOUD_SCRIPTS.md](docs/CLOUD_SCRIPTS.md). Lua API: [docs/LUA_API.md](docs/LUA_API.md). Shaders: [docs/SHADERS.md](docs/SHADERS.md).

Hard rules:
- C++ → Lua calls only via `LuaSandbox::PCall`; never reload/close the Lua state synchronously (use `LuaEngine::RequestReload`).
- Never re-add `os.execute`/`io.popen`/`package`/`debug`/bytecode loading or widen `io.open` (see SECURITY.md). Every native function that takes a pointer from Lua must use `PtrRegistry::Arg`.
- Unity API only on the Unity main thread; the render thread (Present hook) must not touch Unity objects.
- New `.cpp` files must be added to `core/CMakeLists.txt`. Build into a fresh directory (not the repo's `build/`).
- Git: commit without co-author / signature lines (user preference).
