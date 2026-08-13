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
#include "core.h"

namespace konanix::globals::uniform_buffer {

// ---------------------------------------------------------------------------
// uniform buffer object variable definitions
// ---------------------------------------------------------------------------

inline VkBuffer                     buffers             [MAX_FRAMES_IN_FLIGHT]{}; 
inline VkDeviceMemory               memory              [MAX_FRAMES_IN_FLIGHT]{}; 
inline void*                        memory_mapped       [MAX_FRAMES_IN_FLIGHT]{}; 

};
