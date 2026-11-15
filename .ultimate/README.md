# Ultimate setup for gst-c-crash

- **Worktree setup** (`worktree-setup.sh`): every branch gets its own `.venv` (mixtape, current pytest, pytest-asyncio, with system PyGObject/GStreamer) and its own `build/` meson folder.
- **Lander checks**: `ninja -C build`, then pytest with `GST_PLUGIN_PATH=build/gst`. `test_faulty_setup` is skipped because it runs faulty's default method (SIGSEGV), which crashes pytest itself. No push.
- **`failure-verifier` agent**: runs leaky/faulty pipelines under lldb, `leaks` or ASan and confirms each method fails as intended, with evidence.
