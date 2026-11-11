// SPDX-License-Identifier: MIT
// Adapted from OpenRR / DLSSNR-AMD (MIT).
#pragma once

#if defined(__GNUC__) && defined(__x86_64__)
#define RR_NGX_MSABI __attribute__((ms_abi))
#else
#define RR_NGX_MSABI
#endif
