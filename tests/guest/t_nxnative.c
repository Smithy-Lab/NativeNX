// SPDX-License-Identifier: GPL-3.0-or-later
// nxnative's own host sources, built into nxlinux unchanged (compat/nxnative), called the way a game calls them: by
// their mangled SDK names. The SystemEvent case runs the whole chain: guest -> nxnative's x86_64 code -> back into the
// guest's nn::os::CreateSystemEvent (here a stand-in for the SDK's) -> svcCreateEvent -> the guest waits on the event.
#include "nx.h"

typedef struct { u64 v[2]; } Uid;
void acc_init(void) __asm__("_ZN2nn7account10InitializeEv");
Result acc_user_count(int *n) __asm__("_ZN2nn7account12GetUserCountEPi");
Result acc_list_all(int *n, Uid *out, int max) __asm__("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi");
Result acc_nickname(char *nick, const Uid *uid) __asm__("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE");
Result fr_list(int *count, void *out, int off, int n, const void *filter) __asm__("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE");
Result fr_async_event(void *self, void *ev) __asm__("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE");
int sk_poll(void *fds, u64 n, int timeout_ms) __asm__("_ZN2nn6socket4PollEP6pollfdmi");

// stand-ins for the SDK's nn::os SystemEvent functions, which nxnative's hle_bind_system_event calls on the guest
typedef struct { Handle w, r; u32 clear_mode, live; } SystemEventType;
__attribute__((visibility("default"))) Result os_create_system_event(SystemEventType *e, int clear, int interprocess) __asm__("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
__attribute__((visibility("default"))) Result os_create_system_event(SystemEventType *e, int clear, int interprocess) {
    (void)interprocess;
    e->clear_mode = clear, e->live = 1;
    return svcCreateEvent(&e->w, &e->r);
}
__attribute__((visibility("default"))) Handle os_writable(const SystemEventType *e) __asm__("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
__attribute__((visibility("default"))) Handle os_writable(const SystemEventType *e) { return e->w; }

static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

int test_main(void) {
    acc_init();
    int n = -1;
    CHECK("nn::account::GetUserCount (nxnative hle_account.c)", acc_user_count(&n) == 0 && n == 1);
    Uid users[4] = {{{0}}};
    CHECK("nn::account::ListAllUsers: the one local user", acc_list_all(&n, users, 4) == 0 && n == 1 && users[0].v[0] == 0x78626c0000000001ul);
    char nick[33];
    CHECK("nn::account::GetNickname", acc_nickname(nick, &users[0]) == 0 && streq(nick, "Player"));
    int count = -1;
    CHECK("nn::friends::GetFriendList is cut (nxnative hle_friends.c)", fr_list(&count, NULL, 0, 10, NULL) == 0x2A2 && count == 0);

    static SystemEventType ev;
    u64 ctx[8] = {0};
    CHECK("nn::friends AsyncContext::GetSystemEvent", fr_async_event(ctx, &ev) == 0);
    CHECK("... bound the event through the guest's nn::os::CreateSystemEvent", ev.live == 1 && ev.w && ev.r);
    int idx = -1;
    CHECK("... and signaled it: the guest's wait returns at once", svcWaitSync(&idx, &ev.r, 1, 100000000) == 0 && idx == 0);

    u64 t0 = svcGetSystemTick();
    int pr = sk_poll(NULL, 0, 5);
    u64 t1 = svcGetSystemTick();
    CHECK("nn::socket::Poll with nothing to wait on waits out its timeout (nxnative hle_offline.c, hle_net.c)", pr == 0 && t1 - t0 >= 19200 * 4);
    return 0;
}
