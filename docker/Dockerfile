# Build environment for ONE: everything needed to build and test the emulator, the debugger and the menu, check
# the formatting and clang-tidy, regenerate the debugger's quick reference, rebuild the test ROMs and cross-compile
# one.exe for Windows. The source is not copied in: tools/dev.sh mounts the working tree and runs a command here.
#
#   tools/dev.sh cmake --preset linux-debugger && tools/dev.sh cmake --build --preset linux-debugger
#   tools/dev.sh ctest --preset linux-debugger
#   tools/dev.sh            # a shell
#
# Ubuntu 24.04 = the base of Linux Mint 22: the same compilers (GCC 13, Clang 18), CMake 3.28 and SDL 2.30 as a Mint
# development machine, and clang-format/clang-tidy 18, the versions tools/format.sh and tools/tidy.sh require.
# Dear ImGui (and SDL2 for Windows builds) are still downloaded by CMake when it configures.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# No package preconfiguration: it only asks questions (none here, noninteractive), and without apt-utils it prints
# "debconf: delaying package configuration, since apt-utils is not installed" on every install.
RUN echo '#clear DPkg::Pre-Install-Pkgs;' > /etc/apt/apt.conf.d/99no-preconfigure

# Build: compilers, CMake, SDL2 (game window, sound, gamepads), git (CMake's
# FetchContent), Python (test scripts). Lint: clang-format/clang-tidy 18. Clang's runtime libraries and llvm-profdata
# (tools/pgo.sh with Clang, sanitizers with Clang). Test ROMs: cc65 (ca65/ld65).
# Windows cross builds: MinGW-w64, and Wine to run what they build (tools/pgo.sh --windows trains under it). Quick reference: pandoc, fonts (the PDF uses DejaVu) and tmux (screen captures).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates curl gnupg \
        clang-18 clang-format-18 clang-tidy-18 libclang-rt-18-dev llvm-18 \
        libsdl2-dev \
        python3 \
        cc65 \
        gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64 wine wine64 \
        pandoc fonts-dejavu-core fonts-dejavu-extra tmux \
    && rm -rf /var/lib/apt/lists/*

# Chrome prints the quick reference to PDF (docs/quickref/build.sh). Ubuntu's chromium is a snap, which does not
# run in a container, so this is Google's package (x86-64 only; other platforms build without the quick reference).
ARG ONE_QUICKREF=1
RUN if [ "$ONE_QUICKREF" = 1 ] && [ "$(dpkg --print-architecture)" = amd64 ]; then \
        curl -fsSL https://dl.google.com/linux/linux_signing_key.pub \
            | gpg --dearmor -o /usr/share/keyrings/google-chrome.gpg \
        && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/google-chrome.gpg] https://dl.google.com/linux/chrome/deb/ stable main" \
            > /etc/apt/sources.list.d/google-chrome.list \
        && apt-get update && apt-get install -y --no-install-recommends google-chrome-stable \
        && rm -rf /var/lib/apt/lists/*; \
    fi

# clang / clang++ / clang-format / clang-tidy without the -18 suffix too
RUN for tool in clang clang++ clang-format clang-tidy; do ln -sf "/usr/bin/$tool-18" "/usr/local/bin/$tool"; done

# The tests run without a display or sound card: SDL's dummy drivers (the tests that need them set them too).
# HOME is writable for any user id (tools/dev.sh runs as the caller's id so that the files it writes are theirs).
# Chrome's sandbox cannot start in a container: docs/quickref/build.sh runs it without when CHROME_NO_SANDBOX is set.
ENV SDL_VIDEODRIVER=dummy \
    SDL_AUDIODRIVER=dummy \
    CHROME_NO_SANDBOX=1 \
    WINEDEBUG=-all \
    WINEDLLOVERRIDES="mscoree,mshtml=" \
    HOME=/tmp/home
RUN mkdir -m 1777 /tmp/home && git config --system --add safe.directory '*'

CMD ["bash"]
