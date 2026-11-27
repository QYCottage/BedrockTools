#pragma once

#include <cstdint>

#include <bedrocktools/sdk/structs/ItemStackBase.hpp>

namespace bedrocktools::sdk::structs {

// Class: ItemStack
// VTable RVA:   0x13D96B78
// TypeInfo RVA: 0x13D96318
// Virtual Slots: 9
struct ItemStack : public ItemStackBase {
    // [  0] offset + 0x0 | RVA: 0x892BE3C
    virtual void vfunc_0();
    // [  1] offset + 0x8 | RVA: 0x10789E00
    virtual void vfunc_1();
    // [  2] offset +0x10 | RVA: 0x1077074C
    virtual void vfunc_2();
    // [  3] offset +0x18 | RVA: 0x107705A4
    virtual void vfunc_3();
    // [  4] offset +0x20 | RVA: 0x107708C0
    virtual void vfunc_4();
    // [  5] offset +0x28 | RVA: 0x10772704
    virtual void vfunc_5();
    // [  6] offset +0x30 | RVA: 0x911A648
    virtual void vfunc_6();
    // [  7] offset +0x38 | RVA: 0x1077295C
    virtual void vfunc_7();
    // [  8] offset +0x40 | RVA: 0x10772D34
    virtual void vfunc_8();
};

}
