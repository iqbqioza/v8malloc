FROM debian:trixie-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        make \
        cmake \
        gdb \
        valgrind \
        strace \
        ltrace \
        clang \
        clang-format \
        clang-tidy \
        cppcheck \
        man-db \
        sudo \
        git \
        gnupg2 \
        zsh \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 1000 vscode \
    && useradd --uid 1000 --gid 1000 -m vscode \
    && echo 'vscode ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/vscode

RUN chsh -s /usr/bin/zsh vscode

USER vscode
