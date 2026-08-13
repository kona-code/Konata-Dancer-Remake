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
#include <vulkan/vulkan.h>
#define KONANIX_BUILD_WITH_VALIDATION
static constexpr int MAX_FRAMES_IN_FLIGHT = 2;


#ifdef KONANIX_BUILD_WITH_VALIDATION

#define KONANIX_EXTENSIVE_LOGGING
#define KONANIX_GRID_LINE_RENDERING


#ifdef KONANIX_EXTENSIVE_LOGGING
// #define KONANIX_CAMERA_POSITION_LOGGING
// #define KONANIX_MOUSE_MOVEMENT_LOGGING
// #define KONANIX_RUNTIME_OBJECT_LOGGING
#endif

#endif
