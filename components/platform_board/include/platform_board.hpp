#pragma once

#include <cstddef>
#include <cstdint>

#include "platform_board_policy.hpp"

namespace rfbridge {

constexpr char kBoardImageDescriptorMagic[] = "RFBD";
constexpr uint8_t kBoardImageDescriptorVersion = 1;

struct RfBoardImageDescriptor {
    char magic[4];
    uint8_t version;
    uint8_t size;
    uint8_t board_id;
    uint8_t target_id;
    uint8_t flash_mib;
    uint8_t partition_layout_id;
    uint8_t reserved[6];
};

static_assert(sizeof(RfBoardImageDescriptor) == 16);

struct BoardHeapRegionSnapshot {
    uint32_t total = 0;
    uint32_t free = 0;
    uint32_t minimum_free = 0;
    uint32_t largest_free_block = 0;
};

struct BoardMemorySnapshot {
    BoardHeapRegionSnapshot internal{};
    BoardHeapRegionSnapshot psram{};
};

BoardProfile configured_board_profile();
const BoardInfo &current_board_info();
const RfBoardImageDescriptor &current_board_image_descriptor();
BoardMemorySnapshot board_memory_snapshot();
bool board_image_descriptor_is_valid(const RfBoardImageDescriptor &descriptor);
bool board_image_descriptor_is_compatible(const RfBoardImageDescriptor &candidate,
                                          const RfBoardImageDescriptor &running);

}  // namespace rfbridge
