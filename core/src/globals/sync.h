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
#include <vector>

namespace konanix::globals::sync {

// ---------------------------------------------------------------------------
// sync object variable definitions
// ---------------------------------------------------------------------------

inline VkSemaphore                  image_available_semaphores      [MAX_FRAMES_IN_FLIGHT]{};
inline std::vector<VkSemaphore>     render_finished_semaphores      {};
inline VkFence                      in_flight_fences                [MAX_FRAMES_IN_FLIGHT]{};

// stores an in_flight_fence for each image in globals::swapchain::images
inline std::vector<VkFence>         images_in_flight                {};
};
