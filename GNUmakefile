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

# EVERYTHING NOT NAMED IN Makefile.dev MUST STILL REACH THE GENERATED MAKEFILE,
# and this rule is not a convenience -- it is a bug fix.
#
# Shadowing was meant to override ONE target. What it actually did was hide every
# target php-src generates, `install` among them, and a bare `make install` is
# precisely what PIE runs after ./configure. Installing the extension from the
# published package therefore failed with
#
#     make: *** No rule to make target `install'.  Stop.
#
# on a Makefile that had an install target the whole time. `make` itself worked,
# which is what let it ship: Makefile.dev sets .DEFAULT_GOAL and forwards the
# build, so only the second half of the install broke.
#
# .DEFAULT catches any target no rule here defines and forwards it unchanged.
# That keeps the override list exactly as long as Makefile.dev's target list and
# makes this file transparent for everything else -- which is the only safe shape
# for a makefile that sits in front of a generated one.
.DEFAULT:
	@$(MAKE) -f Makefile $@
