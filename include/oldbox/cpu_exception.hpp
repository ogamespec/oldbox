#pragma once
#include "oldbox/common.hpp"
namespace oldbox {
struct CpuException {
    int vector;
    u32 error;
    bool has_error;
};
}
