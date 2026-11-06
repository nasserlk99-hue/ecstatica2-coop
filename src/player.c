/**
 * player.c
 *
 * Local two-player support. The engine has one hero and one set of global key
 * flags, so player 2 is a second instance of the hero actor driven by its own
 * input state. Before behaviour() runs for player 2 we swap the engine's key
 * globals and selected_thing; afterwards everything is restored, leaving the
 * camera, HUD and scripts on player 1.
 *
 * Player 1 input: WASD only (plus the other keys on the left hand), never
 * arrows or numpad — those belong to player 2. Gamepad 0 also drives P1
 * (window_proc already merges it, so it comes through the globals).
 *
 * Player 2 input: the engine's own arrows + numpad mapping, so P2 can be
 * played one-handed on the right side. It also gets a second gamepad when
 * the platform exposes one. It can no longer use T/F/H/K/U/O/Y/J, because in
 * this layout those sit under P1's left hand.
 */

#include "player.h"
#include "display.h"
#include "edit.h"
#include "file.h"
#include "game.h"
#include "init.h"
#include "map.h"
#include "menu.h"
#include "move.h"
#include "music.h"
#include "net.h"
#include "platform.h"
#include "render.h"
#include "topo.h"
#include "win.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

player_t coop_players[COOP_MAX_PLAYERS];
int coop_num_players = 0;

/* Configurable movement keys (PKEY_* values), defaults match the original
 * hard-coded bindings. */
int p1_key_up = 0x11;    /* PKEY_W */
int p1_key_down = 0x1F;  /* PKEY_S */
int p1_key_left = 0x1E;  /* PKEY_A */
int p1_key_right = 0x20; /* PKEY_D */
int p2_key_up = 0xC8;    /* PKEY_UP */
int p2_key_down = 0xD0;  /* PKEY_DOWN */
int p2_key_left = 0xCB;  /* PKEY_LEFT */
int p2_key_right = 0xCD; /* PKEY_RIGHT */
/* Action keys (defaults: P1 run=LShift, jump=LCtrl; P2 run=Num0, jump=NumDec). */
int p1_key_run = 0x2A;   /* PKEY_LSHIFT */
int p1_key_jump = 0x1D;  /* PKEY_LCTRL */
int p1_key_use = 0x39;   /* PKEY_SPACE */
int p2_key_run = 0x52;   /* PKEY_NUM0 */
int p2_key_jump = 0x53;  /* PKEY_NUMDEC */
int p2_key_use = 0x4C;   /* PKEY_NUM5 */
/* Fire (pistol) key. P1: V, P2: Numpad Minus. */
int p1_key_fire = 0x2F;  /* PKEY_V */
int p2_key_fire = 0x4A;  /* PKEY_NUMMINUS */

static bool spawn_pending;
static bool remove_pending;
static bool input_swapped;
static bool split_camera_saved;
static bool player1_follow_mode;
static bool player2_follow_mode;
static bool player1_fire_requested;
static bool player2_fire_requested;
static bool thing_tab_logged;
static uint32_t player1_last_input_time;
static uint32_t player2_last_input_time;
/* E1 run: the Key_F* speed scripts edit each hero's own repertoire (walk
 * slots -> run animations), so the run set is tracked and applied per hero. */
static bool e1_player_run_applied[COOP_MAX_PLAYERS];
static int16_t e1_key_f8_code = -2;
static int16_t e1_key_f12_code = -2;
static void e1_apply_run_mode(actor_t *actor, bool run);
static void update_run_modes(void);
static int16_t p2_camera_index = -1;
static struct {
    camera_data_t *active;
    int16_t selected;
    int32_t zoom;
    vector_t pos, rot;
} split_camera;

static actor_t *saved_selected;
static struct {
    char extra[256];
    char keys[256];
    bool space_pressed, ctrl_pressed, alt_pressed;
    bool key1, key2, key3, key4, key5, key6, key7, key8, key9;
    bool joystick_control;
    int16_t joystick;
    int16_t joy_button;
} saved_keys;

bool coop_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("ECSTATICA_COOP");
        enabled = (e && *e == '0') ? 0 : 1;
    }
    return enabled != 0;
}

static bool actor_in_display_list(const actor_t *actor) {
    for (actor_t *a = root_thing; a; a = a->next_in_display_list)
        if (a == actor) return true;
    return false;
}

static bool actor_in_thing_list(const actor_t *actor) {
    for (actor_t *a = thing_list; a; a = a->next_thing1)
        if (a == actor) return true;
    return false;
}

static bool actor_alive(const actor_t *actor) {
    return actor && (actor_in_display_list(actor) || actor_in_thing_list(actor));
}

bool coop_is_player_actor(const actor_t *actor) {
    if (!actor) return false;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (coop_players[i].active && coop_players[i].actor == actor)
            return true;
    return false;
}

bool coop_is_player2_actor(const actor_t *actor) {
    return actor && coop_players[1].active && coop_players[1].actor == actor;
}

/* Test-only trace: ECSTATICA_COOP_TRACE=1 logs both players' positions,
 * input flags and first part colour twice a second. Used by the automated
 * two-player control test; no effect when the variable is unset. */
static unsigned input_to_bits(const player_input_t *in);

static void coop_trace_positions(void) {
    static int trace = -1;
    static uint32_t last_trace;
    if (trace < 0) {
        const char *e = getenv("ECSTATICA_COOP_TRACE");
        trace = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!trace) return;

    uint32_t now = (uint32_t)my_time();
    if (last_trace && (uint32_t)(now - last_trace) < (uint32_t)(MY_TIME_PER_SEC / 2))
        return;
    last_trace = now;

    actor_t *p1 = coop_players[0].actor;
    actor_t *p2 = coop_players[1].actor;
    if (!p1 || !p2) {
        DBG_LOG(1, "[COOP] trace p1=%p p2=%p active=%d/%d spawn=%d intro=%d\n",
                (void *)p1, (void *)p2, coop_players[0].active, coop_players[1].active,
                spawn_pending, intro_flag);
        return;
    }

    const player_input_t *i1 = &coop_players[0].input;
    const player_input_t *i2 = &coop_players[1].input;
    int c1 = p1->actor_parts_list ? p1->actor_parts_list->color : -1;
    int c2 = p2->actor_parts_list ? p2->actor_parts_list->color : -1;
    int sel = (selected_thing == p1) ? 1 : (selected_thing == p2) ? 2 : 0;
    DBG_LOG(1,
            "[COOP] trace p1=(%d,%d,%d) p2=(%d,%d,%d) "
            "in1=%d%d%d%d in2=%d%d%d%d b1=%u b2=%u eq=%d%d "
            "bh1=%d bh2=%d col1=%d col2=%d "
            "run1=%d run2=%d fol1=%d fol2=%d fs1=%d fs2=%d mt1=%d mt2=%d "
            "rep1=%d rep2=%d "
            "act1=%d act2=%d sel=%d gt=%d\n",
            p1->position_vector.X, p1->position_vector.Y, p1->position_vector.Z,
            p2->position_vector.X, p2->position_vector.Y, p2->position_vector.Z,
            i1->up, i1->down, i1->left, i1->right,
            i2->up, i2->down, i2->left, i2->right,
            input_to_bits(i1), input_to_bits(i2),
            coop_players[0].actor == p1, coop_players[1].actor == p2,
            p1->actor_behavior, p2->actor_behavior, c1, c2,
            /* run1/run2 log the applied auto-run state, not a held key. */
            e1_player_run_applied[0], e1_player_run_applied[1],
            player1_follow_mode, player2_follow_mode,
            coop_players[0].follow_stopped, coop_players[1].follow_stopped,
            p1->move_type, p2->move_type,
            p1->actor_rep_index, p2->actor_rep_index,
            p1->actor_act.act_action != NULL, p2->actor_act.act_action != NULL,
            sel, game_time);
}

bool coop_split_view_begin_player2(void) {
    if (!coop_enabled() || net_is_active() || coop_num_players < 2 || !coop_players[1].active ||
        !coop_players[1].actor || render_backend != RENDER_HARDWARE ||
        !render_map3d || split_camera_saved)
        return false;

    /* Disable split screen in software renderer mode. It only works with the
     * 3D map + hardware renderer. */
    if (render_backend != RENDER_HARDWARE || !render_map3d)
        return false;

    int camera_index = (p2_camera_index > 0) ? p2_camera_index : selected_camera;
    int elem = find_map_element(&coop_players[1].actor->position_vector);
    if (elem >= 0 && elem < 60000) {
        int p2_camera = map_elements[elem].camera_index;
        if (p2_camera > 0 && p2_camera < 1200)
            camera_index = p2_camera;
    }
    if (camera_index <= 0 || camera_index >= 1200) return false;

    split_camera.active = active_camera;
    split_camera.selected = selected_camera;
    split_camera.zoom = zoom_factor;
    copy_vector(&split_camera.pos, &view_pos);
    copy_vector(&split_camera.rot, &view_rot);
    split_camera_saved = true;

    selected_camera = (int16_t)camera_index;
    switch_camera(&camera[camera_index]);
    return true;
}

void coop_split_view_end_player2(void) {
    if (!split_camera_saved) return;
    active_camera = split_camera.active;
    selected_camera = split_camera.selected;
    zoom_factor = split_camera.zoom;
    copy_vector(&view_pos, &split_camera.pos);
    copy_vector(&view_rot, &split_camera.rot);
    calculate_view_matrices();
    split_camera_saved = false;
}

static int16_t direction_index(const player_input_t *in) {
    if (in->up && in->left) return 7;
    if (in->up && in->right) return 1;
    if (in->down && in->left) return 5;
    if (in->down && in->right) return 3;
    if (in->up) return 0;
    if (in->right) return 2;
    if (in->down) return 4;
    if (in->left) return 6;
    return 1000;
}

static void capture_player1_input(void) {
    player_input_t *in = &coop_players[0].input;
    /* Capture before trusting the globals: if a previous frame ended with
     * P2's keys swapped in, these still hold P2's state. */
    if (!input_swapped) {
        /* P1 never uses arrows or numpad — those are P2's. Read P1's keys
         * raw from the platform so that holding P1's Alt is not shadowed by
         * the Windows menu bar. */
        platform_t *p1p = win_platform();
        bool w_down = false, a_down = false, s_down = false, d_down = false;
        bool space = false;
        bool e_down = false, r_down = false, f_down = false, c_down = false;
        if (p1p) {
            w_down = platform_key_down(p1p, p1_key_up);
            a_down = platform_key_down(p1p, p1_key_left);
            s_down = platform_key_down(p1p, p1_key_down);
            d_down = platform_key_down(p1p, p1_key_right);
            e_down = platform_key_down(p1p, PKEY_E);
            r_down = platform_key_down(p1p, PKEY_R);
            f_down = platform_key_down(p1p, PKEY_F);
            c_down = platform_key_down(p1p, PKEY_C);
            space = platform_key_down(p1p, PKEY_SPACE);
        }

        /* E1 two-hand keys are their own actions, not movement, like the
         * original (NUM7/9 quick swing, NUM1/3 pick up). P1's hand keys are
         * E / R for the left hand, F / C for the right hand. */
        in->up = w_down;
        in->down = s_down;
        in->left = a_down;
        in->right = d_down;
        in->use = platform_key_down(p1p, p1_key_use);
        /* E1 auto-runs (see update_run_modes), so the run key stays inert
         * instead of toggling anything; E2 keeps a held run key. */
        in->run = (game_version == GAME_VERSION_E1) ? false
                                                    : platform_key_down(p1p, p1_key_run);
        in->alt = platform_key_down(p1p, PKEY_LALT);
        in->jump = platform_key_down(p1p, p1_key_jump);
        in->pickup_left = e_down;
        in->pickup_right = f_down;
        in->swing_left = r_down;
        in->swing_right = c_down;
        if (p1p && platform_key_hit(p1p, p1_key_fire)) {
            player1_fire_requested = true;
            DBG_LOG(1, "[PISTOL] P1 fire key hit\n");
        }
    }
    /* Gamepad 0 (the one window_proc already merged into the globals) always
     * drives P1, even straight after a botched swap. */
    platform_t *p = win_platform();
    if (p) {
        platform_gamepad_state_t gp;
        if (platform_gamepad_poll_index(p, 0, &gp) && gp.connected) {
            int dz = GAMEPAD_STICK_DEADZONE;
            int ax = gp.left_x < 0 ? -gp.left_x : gp.left_x;
            int ay = gp.left_y < 0 ? -gp.left_y : gp.left_y;
            bool pushed = (int64_t)ax * ax + (int64_t)ay * ay > (int64_t)dz * dz;
            bool stick_vert = pushed && ay * 10 > ax * 24;
            bool stick_horz = pushed && ax * 10 > ay * 24;
            bool stick_diag = pushed && !stick_vert && !stick_horz;

            in->up    |= gp.dpad_up    || ((stick_vert || stick_diag) && gp.left_y > 0);
            in->down  |= gp.dpad_down  || ((stick_vert || stick_diag) && gp.left_y < 0);
            in->left  |= gp.dpad_left  || ((stick_horz || stick_diag) && gp.left_x < 0);
            in->right |= gp.dpad_right || ((stick_horz || stick_diag) && gp.left_x > 0);

            in->use  |= gp.btn_south;
            in->jump |= gp.btn_lb;
            if (game_version != GAME_VERSION_E1)
                in->run |= gp.btn_rb || gp.btn_rt;   /* E1 auto-runs */
            in->alt  |= gp.btn_lt;
            in->pickup_left  |= gp.btn_west;
            in->pickup_right |= gp.btn_north;

            int rs_dz = GAMEPAD_STICK_DEADZONE;
            in->swing_left  |= gp.right_x < -rs_dz;
            in->swing_right |= gp.right_x >  rs_dz;
        }
    }
}

static unsigned input_to_bits(const player_input_t *in) {
    unsigned b = 0;
    if (in->up) b |= 1u << 0;
    if (in->down) b |= 1u << 1;
    if (in->left) b |= 1u << 2;
    if (in->right) b |= 1u << 3;
    if (in->use) b |= 1u << 4;
    if (in->run) b |= 1u << 5;
    if (in->alt) b |= 1u << 6;
    if (in->jump) b |= 1u << 7;
    if (in->pickup_left) b |= 1u << 8;
    if (in->pickup_right) b |= 1u << 9;
    if (in->swing_left) b |= 1u << 10;
    if (in->swing_right) b |= 1u << 11;
    return b;
}

static void bits_to_input(unsigned b, player_input_t *in) {
    memset(in, 0, sizeof(*in));
    in->up = (b & (1u << 0)) != 0;
    in->down = (b & (1u << 1)) != 0;
    in->left = (b & (1u << 2)) != 0;
    in->right = (b & (1u << 3)) != 0;
    in->use = (b & (1u << 4)) != 0;
    in->run = (b & (1u << 5)) != 0;
    in->alt = (b & (1u << 6)) != 0;
    in->jump = (b & (1u << 7)) != 0;
    in->pickup_left = (b & (1u << 8)) != 0;
    in->pickup_right = (b & (1u << 9)) != 0;
    in->swing_left = (b & (1u << 10)) != 0;
    in->swing_right = (b & (1u << 11)) != 0;
}

static void poll_player2_input(void) {
    player_input_t *in = &coop_players[1].input;
    memset(in, 0, sizeof(*in));

    if (net_is_active() && net_mode() == NET_HOST) {
        /* Host: P2 input comes from the network (remote client) */
        bits_to_input(net_state.last_input_bits, in);
        return;
    } else if (net_is_active() && net_mode() == NET_CLIENT) {
        /* Client: local keyboard still drives local player, but we mirror
         * host's world; P2 here is the host's puppet, not local input */
        /* Fall through to local input for client P1? Actually client P1 is
         * local input, P2 is mirrored puppet. So for client, P2 input is
         * not local - it comes from host state. Return empty. */
        return;
    }

    platform_t *p = win_platform();
    if (!p) return;

    /* P2 owns the engine's own arrows + numpad layout, so it can be played
     * one-handed on the right side. It reads the platform directly (not the
     * globals, which hold P1's input). P2 has its OWN modifiers and use key
     * (NUM0/NUM./NUM+/NUM5) so that pressing Space/Ctrl/Shift/Alt for P1
     * never triggers P2's actions. */
    in->up    |= platform_key_down(p, p2_key_up)    || platform_key_down(p, PKEY_NUM8);
    in->down  |= platform_key_down(p, p2_key_down)  || platform_key_down(p, PKEY_NUM2);
    in->left  |= platform_key_down(p, p2_key_left)  || platform_key_down(p, PKEY_NUM4);
    in->right |= platform_key_down(p, p2_key_right) || platform_key_down(p, PKEY_NUM6);
    in->use   |= platform_key_down(p, p2_key_use)   || platform_key_down(p, PKEY_NUM5);
    /* E1 auto-runs (see update_run_modes), so P2's run keys stay inert. */
    if (game_version != GAME_VERSION_E1)
        in->run   |= platform_key_down(p, p2_key_run)   || platform_key_down(p, PKEY_NUM0);
    in->alt   |= platform_key_down(p, PKEY_NUMPLUS);
    in->jump  |= platform_key_down(p, p2_key_jump)  || platform_key_down(p, PKEY_NUMDEC);
    in->pickup_left  |= platform_key_down(p, PKEY_NUM1);
    in->pickup_right |= platform_key_down(p, PKEY_NUM3);
    in->swing_left   |= platform_key_down(p, PKEY_NUM7);
    in->swing_right  |= platform_key_down(p, PKEY_NUM9);
    if (platform_key_hit(p, p2_key_fire)) {
        player2_fire_requested = true;
        DBG_LOG(1, "[PISTOL] P2 fire key hit\n");
    }

    platform_gamepad_state_t gp;
    if (platform_gamepad_poll_index(p, 1, &gp) && gp.connected) {
        int dz = GAMEPAD_STICK_DEADZONE;
        int ax = gp.left_x < 0 ? -gp.left_x : gp.left_x;
        int ay = gp.left_y < 0 ? -gp.left_y : gp.left_y;
        bool pushed = (int64_t)ax * ax + (int64_t)ay * ay > (int64_t)dz * dz;
        bool stick_vert = pushed && ay * 10 > ax * 24;
        bool stick_horz = pushed && ax * 10 > ay * 24;
        bool stick_diag = pushed && !stick_vert && !stick_horz;

        in->up    |= gp.dpad_up    || ((stick_vert || stick_diag) && gp.left_y > 0);
        in->down  |= gp.dpad_down  || ((stick_vert || stick_diag) && gp.left_y < 0);
        in->left  |= gp.dpad_left  || ((stick_horz || stick_diag) && gp.left_x < 0);
        in->right |= gp.dpad_right || ((stick_horz || stick_diag) && gp.left_x > 0);

        in->use  |= gp.btn_south;
        in->jump |= gp.btn_lb;
        if (game_version != GAME_VERSION_E1)
            in->run |= gp.btn_rb || gp.btn_rt;   /* E1 auto-runs */
        in->alt  |= gp.btn_lt;
        in->pickup_left  |= gp.btn_west;
        in->pickup_right |= gp.btn_north;

        int rs_dz = GAMEPAD_STICK_DEADZONE;
        in->swing_left  |= gp.right_x < -rs_dz;
        in->swing_right |= gp.right_x >  rs_dz;
    }
}

static bool player_has_input(const player_input_t *in) {
    return in->up || in->down || in->left || in->right || in->use || in->run ||
           in->alt || in->jump || in->pickup_left || in->pickup_right ||
           in->swing_left || in->swing_right;
}

static bool idle_follow_due(uint32_t last_input_time) {
    uint32_t now = (uint32_t)my_time();
    return (uint32_t)(now - last_input_time) >=
           (3u * (uint32_t)MY_TIME_PER_SEC) / 2u;
}

/* A hero counts as "playing" briefly after the last keypress so a follower
 * can catch up when the leader pauses between actions. Kept much shorter
 * than the idle delay so two idle heroes never start following each other. */
static bool player_recently_active(uint32_t last_input_time) {
    uint32_t now = (uint32_t)my_time();
    return (uint32_t)(now - last_input_time) < (uint32_t)MY_TIME_PER_SEC;
}

/* Test switch: ECSTATICA_COOP_NO_FOLLOW=1 turns the idle auto-follow off so
 * a control test measures only the keys that are actually held. */
static bool coop_follow_disabled(void) {
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("ECSTATICA_COOP_NO_FOLLOW");
        disabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return disabled != 0;
}

static void update_player1_follow_mode(void) {
    if (!coop_players[0].active || !coop_players[0].actor ||
        !coop_players[1].active || !coop_players[1].actor) {
        player1_follow_mode = false;
        return;
    }

    /* The intro drives the hero from scene scripts; taking over with follow
     * would fight the scripted movement. */
    if (intro_flag) {
        player1_follow_mode = false;
        return;
    }

    if (coop_follow_disabled()) {
        player1_follow_mode = false;
        return;
    }

    if (player_has_input(&coop_players[0].input)) {
        player1_last_input_time = (uint32_t)my_time();
        player1_follow_mode = false;
        coop_players[0].follow_stopped = false;
        return;
    }

    /* Mutual idle-follow: P1 trails P2 while P2 is being played. Checking
     * recent input (rather than the 3 s idle timer) means two idle heroes
     * always stand still instead of wandering off together. */
    if (!player_recently_active(player2_last_input_time)) {
        player1_follow_mode = false;
        coop_players[0].follow_stopped = false;
        return;
    }

    player1_follow_mode = idle_follow_due(player1_last_input_time);
}

static void update_player2_follow_mode(void) {
    if (!coop_players[1].active || !coop_players[1].actor) {
        player2_follow_mode = false;
        return;
    }

    /* Player 2 is frozen as BH_EXTERNAL for the intro; never queue follow
     * while the scene scripts are still in charge. */
    if (intro_flag) {
        player2_follow_mode = false;
        return;
    }

    if (coop_follow_disabled()) {
        player2_follow_mode = false;
        return;
    }

    uint32_t now = (uint32_t)my_time();
    if (player_has_input(&coop_players[1].input)) {
        player2_last_input_time = now;
        player2_follow_mode = false;
        coop_players[1].follow_stopped = false;
        return;
    }

    /* Mutual idle-follow: P2 trails P1 while P1 is being played. Checking
     * recent input (rather than the 3 s idle timer) means two idle heroes
     * always stand still instead of wandering off together. */
    if (!player_recently_active(player1_last_input_time)) {
        player2_follow_mode = false;
        coop_players[1].follow_stopped = false;
        return;
    }

    player2_follow_mode = idle_follow_due(player2_last_input_time);
}

static void install_player_input(const player_input_t *in) {
    memset(extra_keys_pressed, 0, 256);
    memset(keys_pressed, 0, 256);

    extra_keys_pressed[72] = in->up;
    extra_keys_pressed[80] = in->down;
    extra_keys_pressed[75] = in->left;
    extra_keys_pressed[77] = in->right;
    extra_keys_pressed[56] = in->alt;
    extra_keys_pressed[57] = in->use;
    extra_keys_pressed[71] = in->swing_left;
    extra_keys_pressed[73] = in->swing_right;
    extra_keys_pressed[79] = in->pickup_left;
    extra_keys_pressed[81] = in->pickup_right;

    keys_pressed[42] = in->jump;

    key8_pressed = in->up;
    key2_pressed = in->down;
    key4_pressed = in->left;
    key6_pressed = in->right;
    key7_pressed = in->up && in->left;
    key9_pressed = in->up && in->right;
    key1_pressed = in->down && in->left;
    key3_pressed = in->down && in->right;
    key5_pressed = false;

    space_pressed = in->use;
    /* E1: ctrl is the directional-attack modifier, not run. Run is handled
     * separately (see update_run_modes), so in E1 this stays clear or the
     * run key would swing an attack instead of sprinting. E2 keeps run. */
    ctrl_pressed = (game_version == GAME_VERSION_E1) ? false : in->run;
    alt_pressed = in->alt;
    joystick = direction_index(in);
    joy_button = in->use ? 1 : 0;
}

bool coop_begin_actor_input(actor_t *actor) {
    bool is_player1 = coop_players[0].active && coop_players[0].actor == actor;
    bool is_player2 = coop_is_player2_actor(actor);
    if (!is_player2 && !(is_player1 && player1_follow_mode)) return false;

    saved_selected = selected_thing;
    memcpy(saved_keys.extra, extra_keys_pressed, sizeof(saved_keys.extra));
    memcpy(saved_keys.keys, keys_pressed, sizeof(saved_keys.keys));
    saved_keys.space_pressed = space_pressed;
    saved_keys.ctrl_pressed = ctrl_pressed;
    saved_keys.alt_pressed = alt_pressed;
    saved_keys.key1 = key1_pressed;
    saved_keys.key2 = key2_pressed;
    saved_keys.key3 = key3_pressed;
    saved_keys.key4 = key4_pressed;
    saved_keys.key5 = key5_pressed;
    saved_keys.key6 = key6_pressed;
    saved_keys.key7 = key7_pressed;
    saved_keys.key8 = key8_pressed;
    saved_keys.key9 = key9_pressed;
    saved_keys.joystick_control = joystick_control;
    saved_keys.joystick = joystick;
    saved_keys.joy_button = joy_button;

    install_player_input(is_player1 ? &coop_players[0].input : &coop_players[1].input);
    selected_thing = actor;
    actor_t *follow_target = is_player1 ? coop_players[1].actor : coop_players[0].actor;
    bool follow = (is_player1 ? player1_follow_mode : player2_follow_mode) && follow_target &&
                  actor->actor_behavior != BH_GET_HIT &&
                  actor->actor_behavior != BH_DYING &&
                  actor->actor_behavior != BH_DEAD &&
                  actor->actor_behavior != BH_RECOVERING;
    joystick_control = !follow;
    if (follow) {
        if (actor->actor_behavior != BH_FOLLOW) {
            actor->interact_state = 0;
            actor->interact_cooldown = 0;
            actor->interact_timer = 0;
        }
        actor->actor_behavior = BH_FOLLOW;
        actor->interact_target_index = follow_target->name_index;
    }
    input_swapped = true;
    return true;
}

void coop_player_try_fire(actor_t *actor) {
    if (!actor || !actor->_PartTab) return;
    /* Use the right hand (field_0[8]) as the firing point. If it holds a
     * weapon, fire from the weapon; otherwise fire from the arm itself. */
    part_t *right_arm = actor->_PartTab->field_0[8];
    part_t *src = right_arm;
    if (right_arm && right_arm->actor_2_held) {
        /* Try to find a part belonging to the held weapon actor so the
         * bullet originates from the weapon's position/orientation. */
        actor_t *weapon = right_arm->actor_2_held;
        if (weapon && weapon->_PartTab && weapon->_PartTab->field_0[0])
            src = weapon->_PartTab->field_0[0];
    }
    if (!src) return;
    fire_bullet(src);
}

bool coop_consume_fire(int player_index) {
    if (player_index == 0) {
        if (player1_fire_requested) { player1_fire_requested = false; return true; }
        return false;
    }
    if (player_index == 1) {
        if (player2_fire_requested) { player2_fire_requested = false; return true; }
        return false;
    }
    return false;
}

void coop_end_actor_input(void) {
    if (!input_swapped) return;
    input_swapped = false;

    memcpy(extra_keys_pressed, saved_keys.extra, sizeof(saved_keys.extra));
    memcpy(keys_pressed, saved_keys.keys, sizeof(saved_keys.keys));
    space_pressed = saved_keys.space_pressed;
    ctrl_pressed = saved_keys.ctrl_pressed;
    alt_pressed = saved_keys.alt_pressed;
    key1_pressed = saved_keys.key1;
    key2_pressed = saved_keys.key2;
    key3_pressed = saved_keys.key3;
    key4_pressed = saved_keys.key4;
    key5_pressed = saved_keys.key5;
    key6_pressed = saved_keys.key6;
    key7_pressed = saved_keys.key7;
    key8_pressed = saved_keys.key8;
    key9_pressed = saved_keys.key9;
    joystick_control = saved_keys.joystick_control;
    joystick = saved_keys.joystick;
    joy_button = saved_keys.joy_button;

    selected_thing = saved_selected;
}

static void remove_player2_now(void) {
    actor_t *p2 = coop_players[1].actor;
    remove_pending = false;
    if (!p2) {
        coop_players[1].active = false;
        return;
    }

    if (actor_alive(p2)) {
        int16_t idx = p2->name_index;
        actor_t *slot = (idx >= 0 && idx < THING_TAB_SIZE) ? thing_tab[idx] : NULL;

        if (actor_in_display_list(p2))
            remove_from_display_list(p2);
        /* do_delete_thing clears thing_tab[name_index]; put player 1 back. */
        do_delete_thing(p2);
        if (idx >= 0 && idx < THING_TAB_SIZE && thing_tab[idx] == NULL &&
            slot != p2)
            thing_tab[idx] = slot;
    }

    coop_players[1].actor = NULL;
    coop_players[1].active = false;
}

static void tint_player2(actor_t *p2) {
    /* Material indices index shade_tab[17], not the palette. A small rotation
     * of the material keeps the second hero recognisably dressed but visibly
     * different. Index 0 is left alone: it is used for transparent/unshaded
     * parts. */
    for (part_t *part = p2->actor_parts_list; part; part = part->next_in_display_list) {
        int m = part->color;
        if (m > 0 && m < 17)
            m = 1 + ((m + 4) % 16);
        part->color = (int16_t)m;
        part->default_color = (int16_t)m;
    }
}

static void spawn_player2(void) {
    spawn_pending = false;

    actor_t *p1 = coop_players[0].actor;
    if (!p1 || !actor_alive(p1)) return;
    if (p1->name_index < 0 || p1->name_index >= THING_TAB_SIZE) return;

    actor_t *p2 = coop_load_actor_instance(p1->name_index);
    if (!p2) {
        DBG_LOG(1, "[COOP] couldn't create player 2 from thing %d\n",
                (int)p1->name_index);
        return;
    }

    /* During the intro the hero is scripted (and players may not control him
     * yet); keep player 2 visible but frozen so he cannot disturb the scene.
     * coop_frame_update switches him to BH_JOYSTICK at handover. */
    p2->actor_behavior = intro_flag ? BH_EXTERNAL : BH_JOYSTICK;
    p2->move_type = -1;
    p2->actor_rep_index = p1->actor_rep_index;
    p2->actor_Speed_factor = p1->actor_Speed_factor;
    p2->actor_strength_factor = p1->actor_strength_factor;
    p2->actor_hit_factor = p1->actor_hit_factor;
    p2->actor_magic_factor = p1->actor_magic_factor;
    p2->actor_magic = p1->actor_magic;
    p2->full_actor_hp = p1->full_actor_hp;
    p2->actor_hitpoints = p1->actor_hitpoints;
    p2->flags |= ACTOR_FLAG_VISIBLE;

    /* One body-width to the hero's right. sine/cos are the forward vector. */
    int16_t yaw = p1->rotate_vector.Y;
    int16_t rx = (int16_t)(cosn_table[(uint16_t)yaw] >> 6);
    int16_t rz = (int16_t)(-sine_table[(uint16_t)yaw] >> 6);

    copy_vector(&p2->position_vector, &p1->position_vector);
    p2->position_vector.X = (int16_t)(p2->position_vector.X + rx);
    p2->position_vector.Z = (int16_t)(p2->position_vector.Z + rz);
    copy_vector(&p2->rotate_vector, &p1->rotate_vector);
    copy_vector(&p2->previous_position, &p2->position_vector);
    copy_vector(&p2->start_position, &p2->position_vector);
    copy_vector(&p2->actor_center, &p2->position_vector);

    int16_t ground = find_height_now(&p2->position_vector, p2);
    if (abs((int)ground - (int)p1->position_vector.Y) < 2000)
        p2->position_vector.Y = ground;
    else
        p2->position_vector.Y = p1->position_vector.Y;
    copy_vector(&p2->previous_position, &p2->position_vector);

    add_to_display_list(p2);
    tint_player2(p2);

    coop_players[1].actor = p2;
    coop_players[1].active = true;
    coop_num_players = 2;
    player2_last_input_time = (uint32_t)my_time();
    player2_follow_mode = false;
    coop_players[1].follow_stopped = false;

    DBG_LOG(1, "[COOP] player 2 spawned: thing=%d at %d,%d,%d (p1 %d,%d,%d)\n",
            (int)p2->name_index,
            p2->position_vector.X, p2->position_vector.Y, p2->position_vector.Z,
            p1->position_vector.X, p1->position_vector.Y, p1->position_vector.Z);
}

void coop_on_hero_loaded(int16_t hero_index) {
    spawn_pending = true;
    if (coop_players[1].actor)
        remove_pending = true;

    coop_players[0].actor =
        (hero_index >= 0 && hero_index < THING_TAB_SIZE) ? thing_tab[hero_index] : NULL;
    coop_players[0].active = coop_players[0].actor != NULL;
    coop_num_players = coop_players[0].active ? 1 : 0;
    player1_last_input_time = (uint32_t)my_time();
    player1_follow_mode = false;
    coop_players[0].follow_stopped = false;
    e1_player_run_applied[0] = false;
    e1_player_run_applied[1] = false;

    DBG_LOG(1, "[COOP] hero loaded: index=%d actor=%p\n",
            (int)hero_index, (void *)coop_players[0].actor);
}

void coop_on_hero_restored(actor_t *hero) {
    if (!hero) return;
    coop_players[0].actor = hero;
    coop_players[0].active = true;
    coop_num_players = 1;
    player1_last_input_time = (uint32_t)my_time();
    player1_follow_mode = false;
    coop_players[0].follow_stopped = false;
    e1_player_run_applied[0] = false;
    e1_player_run_applied[1] = false;
    spawn_pending = true;
    if (coop_players[1].actor)
        remove_pending = true;
    DBG_LOG(1, "[COOP] hero restored: thing=%d\n", (int)hero->name_index);
}

/* fdinhe_rep picks the hero's repertoire from the hero named "fdinhe" (thing
 * slot 0) and reads hands/HP from selected_thing, so point both at this hero
 * for the duration. Player 2 is a second copy of the same thing, and without
 * this the script always rewrites player 1's repertoire. */
static void e1_execute_hero_rep(actor_t *actor, int16_t code_idx) {
    if (!actor || code_idx < 0 || code_idx >= CODE_TAB_SIZE || !code_tab[code_idx])
        return;
    actor_t *saved_thing0 = thing_tab[0];
    actor_t *saved_selected = selected_thing;
    int16_t saved_rep_name0 = actor_rep_name[0];
    thing_tab[0] = actor;
    selected_thing = actor;
    execute_thing_code(actor, code_idx);
    thing_tab[0] = saved_thing0;
    selected_thing = saved_selected;
    actor_rep_name[0] = saved_rep_name0;
}

/* Apply one hero's run (or walk) repertoire. The Key_F* scripts only set scene
 * flags and then run fdinhe_rep, which selects the walk/run set for that hero. */
static void e1_apply_run_mode(actor_t *actor, bool run) {
    if (e1_key_f8_code == -2) {
        e1_key_f8_code = find_code_name_index("Key_F5_8");
        e1_key_f12_code = find_code_name_index("Key_F9_12");
    }
    int16_t ci = run ? e1_key_f12_code : e1_key_f8_code;
    if (ci < 0 || !code_tab[ci]) {
        movement_speed_mode = run ? 8 : 4;
        return;
    }
    e1_execute_hero_rep(actor, ci);
    /* The rep change only takes effect when the movement action restarts, and
     * a held direction keeps move_type, so force this hero to restart the
     * current action now - that makes the switch instant instead of waiting
     * until the next stop or turn. Uninterruptible actions (e.g. pick-ups)
     * are left alone. */
    if (!(actor->flags & 0x40) &&
        !(actor->actor_act.act_action && (actor->actor_act.flags & 0x40))) {
        actor->move_type = 1000;
        actor->actor_act.act_action = NULL;
    }
    DBG_LOG(1, "[COOP] run mode %s for thing %d (code=%d)\n",
            run ? "run" : "walk", (int)actor->name_index, (int)ci);
}

/* E1 auto-run: both heroes always use the run animation/speed set — no key
 * needed. The Key_F* scripts edit each hero's own repertoire, so the run set
 * is applied per hero: once after the intro hands control over, and again
 * whenever a hero (re)spawns with a freshly loaded repertoire. Run keys stay
 * inert; the F1-F12 / L3 speed overrides still work on top of this. */
static void update_run_modes(void) {
    if (game_version != GAME_VERSION_E1 || intro_flag) return;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        actor_t *a = coop_players[i].actor;
        if (!coop_players[i].active || !a) {
            e1_player_run_applied[i] = false;
            continue;
        }
        if (e1_player_run_applied[i]) continue;
        e1_apply_run_mode(a, true);
        e1_player_run_applied[i] = true;
    }
}

void coop_reset_game(void) {
    memset(coop_players, 0, sizeof(coop_players));
    coop_num_players = 0;
    spawn_pending = false;
    remove_pending = false;
    input_swapped = false;
    split_camera_saved = false;
    player1_follow_mode = false;
    player2_follow_mode = false;
    player1_last_input_time = 0;
    player2_last_input_time = 0;
    e1_player_run_applied[0] = false;
    e1_player_run_applied[1] = false;

    /* A menuless autostart (no intro) should still get player 2 as soon as
     * the game loop begins. */
    spawn_pending = true;
}

void coop_frame_update(void) {
    /* Restore selected_thing to P1 if it was changed to P2 in the previous
     * frame for camera purposes. */
    if (coop_players[0].actor && selected_thing != coop_players[0].actor &&
        (selected_thing == coop_players[1].actor)) {
        selected_thing = coop_players[0].actor;
    }

    /* Ensure input is never left swapped from a previous frame, even when
     * coop is enabled. A missed coop_end_actor_input() (e.g. from an early
     * return or a skipped behaviour pass) would otherwise leave P2's key
     * state in the engine globals, making P1 appear stuck pressing keys. */
    if (input_swapped)
        coop_end_actor_input();

    if (!coop_enabled()) {
        return;
    }

    if (remove_pending)
        remove_player2_now();

    /* Recover player 1 if CT_LOAD_HERO was missed (e.g. autostart paths). */
    if (!coop_players[0].actor && selected_thing &&
        selected_thing->actor_behavior == BH_JOYSTICK) {
        coop_on_hero_restored(selected_thing);
    }

    if (coop_players[1].actor && !actor_alive(coop_players[1].actor)) {
        coop_players[1].actor = NULL;
        coop_players[1].active = false;
        coop_num_players = coop_players[0].active ? 1 : 0;
        spawn_pending = true;
    }

    /* Camera: make the shared camera follow P2 when P2 is not in P1's zone. */
    if (coop_players[1].active && coop_players[1].actor && selected_thing) {
        int p2_elem = find_map_element(&coop_players[1].actor->position_vector);
        int p1_elem = find_map_element(&selected_thing->position_vector);
        if (p2_elem >= 0 && p1_elem >= 0) {
            int p2_cam = map_elements[p2_elem].camera_index;
            int p1_cam = map_elements[p1_elem].camera_index;
            if (p2_cam > 0 && p2_cam < 1200 && p2_cam != p1_cam) {
                bool p2_moving = coop_players[1].input.up || coop_players[1].input.down ||
                                 coop_players[1].input.left || coop_players[1].input.right;
                /* Follow P2 whenever P2 moves */
                if (p2_moving) {
                    selected_thing = coop_players[1].actor;
                }
            }
        }
    }

    capture_player1_input();
    poll_player2_input();
    update_player1_follow_mode();
    update_player2_follow_mode();
    update_run_modes();

    /* Manual P1 input always retakes the selected-actor slot, even if the
     * shared-camera handoff selected P2 on the previous frame. */
    if (coop_players[0].active &&
        player_has_input(&coop_players[0].input))
        selected_thing = coop_players[0].actor;

    /* Re-assert player 1's keys. collect_behaviour (get_mouse/window_proc)
     * may run other input readers before the first behaviour pass of the
     * frame (present_delay, menus), and a path that returns early from the
     * main loop can leave the globals swapped to player 2. The engine reads
     * keyboard/cursor/Alt state for its own UI first, then P1's behaviour()
     * reads it — so the freshest copy wins for P1. */
    if (coop_players[0].active)
        install_player_input(&coop_players[0].input);

    /* Spawn as soon as the hero exists, including the intro: player 2 should
     * load with player 1. He stays BH_EXTERNAL (and so ignores input) until
     * the intro hands control over. */
    if (spawn_pending && coop_players[0].actor)
        spawn_player2();

    if (coop_players[1].active && coop_players[1].actor &&
        !intro_flag && coop_players[1].actor->actor_behavior == BH_EXTERNAL) {
        coop_players[1].actor->actor_behavior = BH_JOYSTICK;
        /* Start the idle-follow clock at handover, not from the start of the
         * intro, so neither hero immediately walks off on its own. */
        player1_last_input_time = (uint32_t)my_time();
        player2_last_input_time = player1_last_input_time;
        player1_follow_mode = false;
        player2_follow_mode = false;
        coop_players[0].follow_stopped = false;
        coop_players[1].follow_stopped = false;
    }

    /* Track P2's camera index so split-screen view follows P2's movement. */
    if (coop_players[1].active && coop_players[1].actor) {
        int elem = find_map_element(&coop_players[1].actor->position_vector);
        if (elem >= 0 && elem < 60000) {
            int cam = map_elements[elem].camera_index;
            if (cam > 0 && cam < 1200)
                p2_camera_index = (int16_t)cam;
        }
    }

    coop_trace_positions();

    /* NOTE: selected_thing may have been changed to P2 above for camera
     * purposes. We do NOT restore it here — the render pass (check_camera)
     * needs to see the current selected_thing. It is restored at the start
     * of the next coop_frame_update() call. */

    /* Network: after local input is captured, push it to host (client) or
     * pull host state (client) / broadcast (host). */
    if (net_is_active()) {
        if (net_mode() == NET_HOST) {
            net_broadcast_state(game_time);
        } else if (net_mode() == NET_CLIENT) {
            net_send_input(input_to_bits(&coop_players[0].input));
            /* Client is remote player. Freeze both local actors to pure
             * puppets and smooth them toward the host's authoritative state. */
            if (net_state.connected) {
                actor_t *selfp = coop_players[1].actor;
                actor_t *other = coop_players[0].actor;
                if (selfp && selfp->actor_behavior != BH_EXTERNAL)
                    selfp->actor_behavior = BH_EXTERNAL;
                if (other && other->actor_behavior != BH_EXTERNAL)
                    other->actor_behavior = BH_EXTERNAL;
                if (other && (net_state.p1_x | net_state.p1_z)) {
                    other->position_vector.X = (int16_t)(other->position_vector.X + (net_state.p1_x - other->position_vector.X) / 4);
                    other->position_vector.Y = (int16_t)(other->position_vector.Y + (net_state.p1_y - other->position_vector.Y) / 4);
                    other->position_vector.Z = (int16_t)(other->position_vector.Z + (net_state.p1_z - other->position_vector.Z) / 4);
                    other->rotate_vector.X = (int16_t)(other->rotate_vector.X + (net_state.p1_rx - other->rotate_vector.X) / 4);
                    other->rotate_vector.Y = (int16_t)(other->rotate_vector.Y + (net_state.p1_ry - other->rotate_vector.Y) / 4);
                    other->rotate_vector.Z = (int16_t)(other->rotate_vector.Z + (net_state.p1_rz - other->rotate_vector.Z) / 4);
                }
                if (selfp) {
                    int tx = net_state.puppet_x, ty = net_state.puppet_y, tz = net_state.puppet_z;
                    selfp->position_vector.X = (int16_t)(selfp->position_vector.X + (tx - selfp->position_vector.X) / 4);
                    selfp->position_vector.Y = (int16_t)(selfp->position_vector.Y + (ty - selfp->position_vector.Y) / 4);
                    selfp->position_vector.Z = (int16_t)(selfp->position_vector.Z + (tz - selfp->position_vector.Z) / 4);
                    selfp->rotate_vector.X = (int16_t)(selfp->rotate_vector.X + (net_state.puppet_rx - selfp->rotate_vector.X) / 4);
                    selfp->rotate_vector.Y = (int16_t)(selfp->rotate_vector.Y + (net_state.puppet_ry - selfp->rotate_vector.Y) / 4);
                    selfp->rotate_vector.Z = (int16_t)(selfp->rotate_vector.Z + (net_state.puppet_rz - selfp->rotate_vector.Z) / 4);
                }
            }
        }
        net_frame_update();
    }
}
