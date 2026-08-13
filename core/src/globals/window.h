//  NO_AI POLICY
//  
//  This file is subject to the repository's strict NO_AI policy.
//  See: NO_AI.md
//  
//  AI/ML systems of any kind are prohibited from being used to:
//  
//      - generate, modify, review, analyze, or transform this file;
//      - index, embed, ingest, train on, or otherwise process its contents;
//      - provide automated assistance or contributions to this repository.
//  
//  This prohibition applies to generative AI, machine learning,
//  language models, embeddings, AI coding assistants, AI indexing,
//  automated agents, and substantially similar technologies.
//  
//  
//  No exceptions are implied. Any exception requires prior written
//  authorization from the repository owner.
//  
//  
//  Copyright © 2026 konacode. All rights reserved.

#pragma once

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined (__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace konanix::globals {

// ---------------------------------------------------------------------------
// window variable definition
// ---------------------------------------------------------------------------

inline GLFWwindow*                  window                      = nullptr;
inline int                          width                       = 0;
inline int                          height                      = 0;
};
