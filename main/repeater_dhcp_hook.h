#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct dhcps_msg;

s16_t repeater_lwip_dhcps_post_state(struct dhcps_msg *msg, u16_t len, s16_t state);

#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state) \
    repeater_lwip_dhcps_post_state((msg), (len), (state))

#ifdef __cplusplus
}
#endif
