#!/usr/bin/env bash

# wait for the password prompt "read(4" and hit Ctrl+C

# expected: unrar should stop with the output
# User break
# Program aborted

set -x
set -e

unrar="$1"
# if [ -z "$unrar" ]; then unrar="unrar"; fi
if [ -z "$unrar" ]; then
  unrar="$(
    NIXPKGS_ALLOW_UNFREE=1 \
    nix-build -E '
      with import <nixpkgs> { };
      (unrar.overrideAttrs (oldAttrs: {
        # src = /home/user/src/milahu/unrar-ctrl-c-on-password-prompt/unrar;
        src = ./.;
        # sourceRoot = "unrar";
        sourceRoot = ".";
      }))
    '
  )/bin/unrar"
fi

# create archive contents
if ! [ -e test.txt ]; then
  echo test >test.txt
fi

# create archive
if ! [ -e test.rar ]; then
  rar a -ptest test.rar test.txt
fi

# extract archive
strace -f -e trace=rt_sigaction,rt_sigprocmask,read -e signal=SIGINT \
"$unrar" x test.rar
