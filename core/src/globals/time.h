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
#include <chrono>

namespace konanix::globals::time {

// ---------------------------------------------------------------------------
// time variable definitions
// ---------------------------------------------------------------------------

static const std::chrono::time_point<std::chrono::high_resolution_clock, std::chrono::duration<long, std::ratio<1,1000000000>>> start = std::chrono::high_resolution_clock::now();
inline std::chrono::time_point<std::chrono::high_resolution_clock, std::chrono::duration<long, std::ratio<1,1000000000>>>       last_frame;
inline float delta_time;
inline float elapsed {};

};
