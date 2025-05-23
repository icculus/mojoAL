# MojoAL

# What is this?

MojoAL is a full [OpenAL](https://openal.org/) 1.1 implementation, written
in C, in a single source file. It uses Simple Directmedia Layer (SDL) 2.0
to handle much of the heavy lifting and platform abstractions, allowing
you to have a simple, portable OpenAL on any platform that SDL supports,
from desktops to phones to web browsers to the Nintendo Switch.

This project can be compiled directly into an app, or built as a shared
library, making it a drop-in replacement for other OpenAL implementations.

All of core OpenAL 1.1 is supported, including audio capture (recording)
and multiple device support. A handful of popular extensions are also included.

## SDL3

This SDL2-based branch of MojoAL is in maintenance mode. We are actively
working on the SDL3 version in the main branch. We'll accept bug reports and
patches for the SDL2 version, but we'd encourage you to consider moving to
SDL3.