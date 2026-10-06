/*
 * Host lifecycle test for the OBD2 BLE central (main/boost_obd_ble.c).
 *
 * Drives the REAL driver black-box (boost_obd_ble_init/host_start/start/stop/
 * state) against the fake NimBLE host in tools/nimble_fake/, and asserts the
 * three invariants of the panel OFF/ON race:
 *
 *   1. plain off -> on: a disabled-then-enabled link reaches READY again.
 *   2. the race: a connect procedure that completes while the driver is
 *      disabled (and whose CONNECT event is therefore ignored) must not leave
 *      the central permanently unable to reach READY after re-enable.
 *   3. no phantom link: no live NimBLE connection may survive a disable.
 *
 * The fake models the six primary-source NimBLE facts (see nimble_fake.c).
 * Timing is real: the driver's own OBD_RECONNECT_MS (10 s) post-disconnect
 * delay is part of its behaviour, so a few bounded waits are seconds long.
 * A hard alarm cap turns any hang into a nonzero exit.
 */
#include "boost_obd_ble.h"
#include "nimble_fake.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HARD_CAP_S 120

static int g_failures;
static bool g_check1_stop_clean;   /* invariant 3, plain-off->on stop */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(uint32_t ms) { usleep((useconds_t)ms * 1000u); }

static const char *state_name(boost_obd_ble_state_t s)
{
    switch (s) {
    case BOOST_OBD_BLE_DOWN:         return "DOWN";
    case BOOST_OBD_BLE_SCANNING:     return "SCANNING";
    case BOOST_OBD_BLE_CONNECTING:   return "CONNECTING";
    case BOOST_OBD_BLE_DISCOVERING:  return "DISCOVERING";
    case BOOST_OBD_BLE_READY:        return "READY";
    case BOOST_OBD_BLE_DISCONNECTED: return "DISCONNECTED";
    default:                         return "?";
    }
}

/* Rich per-step diagnostic: driver state, peer, last NimBLE error, the fake's
 * live connection table, the last rc ble_gap_connect() produced, and whether a
 * procedure/scan is in flight. */
static void diag(const char *where)
{
    char table[160];
    fake_conn_table_str(table, sizeof(table));
    printf("[diag] %-42s state=%-12s peer=\"%s\"/\"%s\" last_err=0x%04x "
           "conns=%s last_connect_rc=%s pending=%d scan=%d\n",
           where, state_name(boost_obd_ble_state()),
           boost_obd_ble_peer_name(), boost_obd_ble_peer_addr(),
           (unsigned)boost_obd_ble_last_error(), table,
           fake_rc_str(fake_last_connect_rc()),
           (int)fake_conn_pending(), fake_scan_active());
    fflush(stdout);
}

static bool wait_ready(uint32_t timeout_ms)
{
    const uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (boost_obd_ble_state() == BOOST_OBD_BLE_READY) return true;
        if (mono_ms() >= deadline) return false;
        usleep(2000);
    }
}

static bool wait_state(boost_obd_ble_state_t want, uint32_t timeout_ms)
{
    const uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (boost_obd_ble_state() == want) return true;
        if (mono_ms() >= deadline) return false;
        usleep(2000);
    }
}

/*
 * A live-link teardown delivers DISCONNECT asynchronously; the driver then
 * spends OBD_RECONNECT_MS (10 s) inside its DISCONNECTED handler before it
 * accepts the next OBD_EV_START. Wait for the DISCONNECTED state, then for the
 * handler to finish, so the next start() is processed against an idle driver
 * (and so the handler's own post-delay retry does not race the test's start).
 */
static void settle_after_disconnect(void)
{
    (void)wait_state(BOOST_OBD_BLE_DISCONNECTED, 3000);
    sleep_ms(11000);
}

static bool ensure_ready(void)
{
    if (boost_obd_ble_state() == BOOST_OBD_BLE_READY) return true;
    fake_set_auto_connect(true);
    boost_obd_ble_start();
    return wait_ready(15000);
}

/* Precondition: a first link comes up. */
static bool scenario_bringup(void)
{
    fake_set_auto_connect(true);
    boost_obd_ble_start();
    const bool ready = wait_ready(8000);
    diag("bringup: first link");
    return ready;
}

/* Invariant 1: plain off -> on reaches READY again. */
static bool scenario_plain_off_on(void)
{
    const uint64_t t0 = mono_ms();
    if (boost_obd_ble_state() != BOOST_OBD_BLE_READY) {
        diag("check-1: precondition not READY");
        return false;
    }

    boost_obd_ble_stop();
    g_check1_stop_clean = fake_wait_conn_count(0, 3000);
    diag("check-1: disabled, table checked");
    settle_after_disconnect();
    diag("check-1: settled");

    boost_obd_ble_start();
    const bool ready = wait_ready(8000);
    diag("check-1: after re-enable");
    printf("[time] check-1 took %llu ms\n", (unsigned long long)(mono_ms() - t0));
    return ready;
}

/*
 * Invariant 2 (race) + invariant 3 (no phantom link).
 *
 * Precondition: a live link (mid-drive). We drop it, start a fresh connect
 * procedure, disable while that procedure is in flight, then let the controller
 * complete it -- the CONNECT event lands in the disabled window. A correct
 * driver must not keep that link (invariant 3) and must reach READY after
 * re-enable (invariant 2).
 */
static void scenario_race(bool *ok2, bool *ok3)
{
    *ok2 = false;
    *ok3 = false;
    const uint64_t t0 = mono_ms();

    if (!ensure_ready()) { diag("race: precondition not READY"); return; }
    diag("race: pre (link established)");

    fake_set_auto_connect(false);   /* the test owns when the connect completes */

    /* Drop the live link so the next start() begins a directed connect. */
    boost_obd_ble_stop();
    const bool cleared = fake_wait_conn_count(0, 3000);
    settle_after_disconnect();
    diag("race: link cleared");

    boost_obd_ble_start();
    const bool pending = fake_wait_conn_pending(3000);
    diag("race: connect procedure in flight");

    /* Disable while the procedure is in flight. The driver's STOP path has no
     * s_conn_handle to terminate, so it can only ble_gap_disc_cancel() +
     * ble_gap_conn_cancel() -- which must NOT tear down a link. */
    boost_obd_ble_stop();
    (void)wait_state(BOOST_OBD_BLE_DOWN, 2000);
    diag("race: disabled with connect in flight");

    /* Controller wins the cancel race: the connection completes into the
     * disabled window (NimBLE has no cleanup for an ignored CONNECT). */
    const bool completed = fake_conn_complete_now();
    diag("race: connect completed in disabled window");
    sleep_ms(300);                  /* let the driver consume OBD_EV_CONNECTED */
    diag("race: CONNECTED processed");

    /* Invariant 3. */
    const bool no_phantom = fake_wait_conn_count(0, 1500);
    diag("race: phantom-link check");
    *ok3 = pending && completed && no_phantom;

    /* Invariant 2: re-enable must reach READY again. */
    const bool terminated = (fake_conn_count() == 0);
    if (terminated) {
        settle_after_disconnect();
    } else {
        sleep_ms(500);
    }
    boost_obd_ble_start();
    const bool pending2 = fake_wait_conn_pending(3000);
    if (pending2) (void)fake_conn_complete_now();
    const bool ready = wait_ready(8000);
    diag("race: after re-enable");
    *ok2 = pending2 && ready;

    printf("[time] race took %llu ms (cleared=%d pending=%d completed=%d "
           "phantom=%d terminated=%d pending2=%d ready=%d)\n",
           (unsigned long long)(mono_ms() - t0), (int)cleared, (int)pending,
           (int)completed, (int)no_phantom, (int)terminated, (int)pending2,
           (int)ready);
    fflush(stdout);
}

static void report(const char *label, bool ok)
{
    printf("obd-ble-lifecycle: %-78s: %s\n", label, ok ? "OK" : "FAIL");
    if (!ok) g_failures++;
    fflush(stdout);
}

static void on_alarm(int sig)
{
    (void)sig;
    const char *msg = "obd-ble-lifecycle: FAIL (hard timeout, driver hung)\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    (void)n;
    _exit(2);
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(HARD_CAP_S);

    printf("== obd-ble-lifecycle: real boost_obd_ble.c on fake NimBLE ==\n");

    const ble_addr_t peer = {
        .type = BLE_ADDR_PUBLIC,
        .val = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 },
    };

    fake_reset();
    fake_nvs_seed_peer(&peer);      /* load_peer() finds a stored peer */
    boost_obd_ble_init();
    boost_obd_ble_host_start();

    const bool boot = scenario_bringup();

    bool ok1 = false, ok2 = false, ok3 = false;
    if (boot) {
        ok1 = scenario_plain_off_on();
        scenario_race(&ok2, &ok3);
    } else {
        diag("aborting: bring-up failed");
    }

    printf("\n");
    report("precondition: initial link reaches READY", boot);
    report("invariant-1: plain stop->start reaches READY again", ok1);
    report("invariant-2: connect completing in the disabled window still reaches READY", ok2);
    /* Invariant 3 spans every stop: the plain off->on stop (check-1) and the
     * race's disabled-window stop. */
    const bool no_phantom_any_stop = ok3 && g_check1_stop_clean;
    report("invariant-3: no connection survives boost_obd_ble_stop()", no_phantom_any_stop);

    const bool pass = boot && ok1 && ok2 && no_phantom_any_stop;
    printf("obd-ble-lifecycle: %s\n", pass ? "PASS" : "FAIL");
    fflush(stdout);
    alarm(0);
    return pass ? 0 : 1;
}
