#pragma once
#define STACK_ALIGNMENT 16u
#define STACK_ALIGN_UP(x) (((x)+15u)&~(uintptr_t)15u)
