#!/bin/bash
git rebase -i HEAD~4 << 'REBASE_EOF'
edit 767594d build: fix CMake configs, add web_status_map to host tests, and setup SDK for web server (P3-3a)
edit 73b5335 feat(web_cert): add self-signed cert generation with PSA Crypto (P3-3a)
edit 0793bdb feat(web_server): add HTTPS server and APIs for setup, login, logout, status (P3-3a)
pick a769eec docs(web): add WEB_API.md for API specifications (P3-3a)
REBASE_EOF
