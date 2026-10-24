// secrets.h when it exists (local builds), else the placeholders (CI).
#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif
