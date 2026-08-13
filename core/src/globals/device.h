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
#include <stdexcept>

namespace konanix::globals::device {

// ---------------------------------------------------------------------------
// device variable definitions
// ---------------------------------------------------------------------------

inline VkPhysicalDevice             physical_device             = nullptr;
inline VkDevice                     device                      = nullptr;
inline uint32_t                     queue_family                = (uint32_t)-1;
inline VkQueue                      graphics_queue              = nullptr;
inline VkQueue                      present_queue               = nullptr;

static uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(globals::device::physical_device, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type");
}



};
