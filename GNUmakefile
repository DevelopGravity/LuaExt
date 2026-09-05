# GNU make reads GNUmakefile before Makefile, so THIS is what a bare
# `make <target>` finds in a configured checkout. That is deliberate, and it buys
# two things:
#
#   1. `make check`, `make dev` and the rest of Makefile.dev work without anyone
#      having to symlink anything first.
#
#   2. `make clean` is SCOPED. The clean target in the generated ./Makefile is
#      copied verbatim out of php-src's Makefile.global and runs
#
#          find . -name \*.lo -o -name \*.o -o -name \*.dep | xargs rm -f
#
#      from the repository root, with no idea that .gitignore exists. It walks
#      into .tools/, .venv/, a worktree, anything parked in the tree -- and it
#      deleted a 300 MB php-src build here once. Makefile.global is a file the
#      PHP installation owns, it offers no clean-local hook, and editing the
#      generated ./Makefile does not survive the next ./configure. Shadowing the
#      target is the only durable fix.
#
# The generated Makefile is still there and still authoritative for the build
# itself; every target below that needs it calls it with an explicit -f Makefile.
# To reach it directly for anything Makefile.dev does not wrap:
#
#     make -f Makefile <target>
#
# including `make -f Makefile clean`, which is the unscoped one.

include Makefile.dev
