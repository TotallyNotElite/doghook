#include <precompiled.hh>

#include "aimbot.hh"

#include "backtrack.hh"

#include "sdk/class_id.hh"
#include "sdk/entity.hh"
#include "sdk/interface.hh"
#include "sdk/player.hh"
#include "sdk/sdk.hh"
#include "sdk/weapon.hh"

#include "sdk/convar.hh"

#include "sdk/log.hh"
#include "utils/math.hh"
#include "utils/profiler.hh"

#include <algorithm>

using namespace sdk;

namespace aimbot {
struct Target {
    Entity *     e;
    math::Vector v;
    u32          cmd_delta;
};

std::vector<Target> targets;

bool can_find_targets = false;

Player *local_player;
Weapon *local_weapon;

int          local_team;
math::Vector local_view;

// Check it a point is visible to the player
static auto visible_no_entity(const math::Vector &position) {
    trace::TraceResult result;
    trace::Ray         ray;
    trace::Filter      f(local_player);

    ray.init(local_view, position);

    iface::trace->trace_ray(ray, 0x46004003, &f, &result);

    return result.fraction == 1.0f;
}

static Convar<bool> doghook_aimbot_pedantic_mode{"doghook_aimbot_pedantic_mode", true, nullptr};
static auto         visible(Entity *e, const math::Vector &position, const int hitbox) {
    profiler_profile_function();

    trace::TraceResult result;
    trace::Ray         ray;
    trace::Filter      f(local_player);

    ray.init(local_view, position);

    iface::trace->trace_ray(ray, 0x46004003, &f, &result);

    if (doghook_aimbot_pedantic_mode == true) {
        if (result.entity == e && result.hitbox == hitbox) return true;
    } else if (result.entity == e) {
        return true;
    }

    return false;
}

static Convar<float> doghook_aimbot_multipoint_granularity{"doghook_aimbot_multipoint_granularity", 0, 0, 10, nullptr};

static auto multipoint_internal(Entity *e, u32 count, float granularity, const int hitbox, const math::Vector &centre,
                                const math::Vector &min, const math::Vector &max, math::Vector &out) {
    profiler_profile_function();

    // go from centre to centre min first
    for (u32 i = 1; i < count; i++) {
        auto         percent = granularity * i;
        math::Vector point   = centre.lerp(min, percent);

        if (visible(e, point, hitbox)) {
            out = point;
            return true;
        }
    }

    // now from centre to max
    for (u32 i = 1; i < count; i++) {
        auto         percent = granularity * i;
        math::Vector point   = centre.lerp(max, percent);

        if (visible(e, point, hitbox)) {
            out = point;
            return true;
        }
    }

    return false;
}

// TODO: there must be some kind of better conversion we can use here to get a straight line across the hitbox
static auto multipoint(Player *player, const int hitbox, const math::Vector &centre, const math::Vector &min, const math::Vector &max, math::Vector &position_out) {
    profiler_profile_function();

    // create a divisor out of the granularity
    float divisor = doghook_aimbot_multipoint_granularity;
    if (divisor <= 1) return false;
    float granularity = 1.0f / divisor;

    auto new_x = math::lerp(0.5, min.x, max.x);

    // Create a horizontal cross shape out of this box instead of top left bottom right or visa versa
    math::Vector centre_min_x = math::Vector(new_x, min.y, centre.z);
    math::Vector centre_max_x = math::Vector(new_x, max.y, centre.z);

    if (multipoint_internal(player, divisor, granularity, hitbox, centre, centre_min_x, centre_max_x, position_out))
        return true;

    auto new_y = math::lerp(0.5, min.x, max.x);

    math::Vector centre_min_y = math::Vector(min.x, new_y, centre.z);
    math::Vector centre_max_y = math::Vector(max.x, new_y, centre.z);

    if (multipoint_internal(player, divisor, granularity, hitbox, centre, centre_min_y, centre_max_y, position_out))
        return true;

    return false;
}

static auto find_best_box() {
    auto tf_class        = local_player->tf_class();
    auto weapon_class_id = local_weapon->client_class()->class_id;

    switch (tf_class) {
    case 2:                                                                              // sniper
        if (weapon_class_id == class_id::CTFSniperRifle) return std::make_pair(0, true); // aim head with the rifle
    default:
        return std::make_pair(3, false); // chest
    }
}

static Convar<bool> doghook_aimbot_enable_backtrack{"doghook_aimbot_enable_backtrack", true, nullptr};
static Convar<bool> doghook_aimbot_reverse_backtrack_order{"doghook_aimbot_reverse_backtrack_order", false, nullptr};

auto visible_target_inner(Player *player, std::pair<int, bool> best_box, u32 tick, math::Vector &pos) {
    PlayerHitboxes hitboxes;
    u32            hitboxes_count;

    auto [best_hitbox, only_use_best] = best_box;

    hitboxes_count = backtrack::hitboxes_for_player(player, tick, hitboxes);

    // check best hitbox first
    if (visible(player, hitboxes.centre[best_hitbox], best_hitbox)) {
        pos = hitboxes.centre[best_hitbox];
        return true;
    } else if (multipoint(player, best_hitbox, hitboxes.centre[best_hitbox], hitboxes.min[best_hitbox], hitboxes.max[best_hitbox], pos)) {
        return true;
    }

    // .second is whether we should only check the best box
    if (!only_use_best) {
        for (u32 i = 0; i < hitboxes_count; i++) {
            if (visible(player, hitboxes.centre[i], i)) {
                pos = hitboxes.centre[i];
                return true;
            }
        }

#if 0
        // TODO: Perform multiboxing after confirming that we do not have any other options
        for (u32 i = 0; i < hitboxes_count; i++) {
            if (multipoint(player, i, hitboxes.centre[i], hitboxes.min[i], hitboxes.max[i], pos)) {
                return true;
            }
        }
#endif
    }

    return false;
}

auto visible_player(Player *p, std::pair<int, bool> &best_box, u32 tick, math::Vector &pos) {
    profiler_profile_function();

    auto visible = visible_target_inner(p, best_box, tick, pos);
    if (visible) return true;

    return false;
}

auto valid_target(Entity *e) {
    if (auto player = e->to_player()) {
        if (!player->alive()) return false;
        if (local_team == player->team()) return false;

        return true;
    }

    return false;
}

void finished_target(Target t) {
    targets.push_back(t);
}

auto sort_targets() {
    profiler_profile_function();

    std::sort(targets.begin(), targets.end(),
              [](const Target &a, const Target &b) {
                  return a.v.distance(local_view) < b.v.distance(local_view);
              });
}

auto find_targets() {
    profiler_profile_function();

    auto best_box = find_best_box();

    auto find_target_inner = [&best_box](u32 tick, u32 delta) {
        for (auto e : iface::ent_list->get_range()) {
            if (!e->is_valid()) continue;

            if (!valid_target(e)) continue;

            auto pos = math::Vector::invalid();

            if (auto p = e->to_player()) {
                if (!visible_player(p, best_box, tick, pos)) continue;

                finished_target(Target{e, pos, delta});

                // TODO: only do this when we want to do speedy targets!
                //break;
            }
        }
    };

    auto current_tick = iface::globals->tickcount;

    bool reverse_order = doghook_aimbot_reverse_backtrack_order;

    // Easy out
    if (!doghook_aimbot_enable_backtrack) {
        find_target_inner(current_tick, 0);
        if (targets.size() > 0) sort_targets();

        return;
    }

    // Change
    const auto delta_delta = reverse_order ? -1 : 1;

    // Starting position
    auto delta = reverse_order ? backtrack::max_ticks : 1;

    backtrack::RewindState rewind;

    do {
        u32 new_tick = current_tick - delta;

        if (backtrack::tick_valid(new_tick)) {
            rewind.to_tick(new_tick);

            find_target_inner(new_tick, delta);

            // we found some targets for this state... stop
            if (targets.size() > 0) break;
        }

        // Move to the next tick
        delta += delta_delta;
        new_tick = current_tick - delta;
    } while (delta > 0 && delta < backtrack::max_ticks);

    sort_targets();
}

// TODO: move this outside of aimbot
// Other modules may mess the angles up aswell
// So our best bet is to run this at the end of createmove...
inline static auto clamp_angle(const math::Vector &angles) {
    math::Vector out;

    out.x = angles.x;
    out.y = angles.y;
    out.z = 0;

    while (out.x > 89.0f) out.x -= 180.0f;
    while (out.x < -89.0f) out.x += 180.0f;

    while (out.y > 180.0f) out.y -= 360.0f;
    while (out.y < -180.0f) out.y += 360.0f;

    out.y = std::clamp(out.y, -180.0f, 180.0f);
    out.x = std::clamp(out.x, -90.0f, 90.0f);

    return out;
}

inline static auto fix_movement_for_new_angles(const math::Vector &movement, const math::Vector &old_angles, const math::Vector &new_angles) {
    profiler_profile_function();

    math::Matrix3x4 rotate_matrix;

    auto delta_angles = new_angles - old_angles;
    delta_angles      = clamp_angle(delta_angles);

    rotate_matrix.from_angle(delta_angles);
    return rotate_matrix.rotate_vector(movement);
}

// TODO: something something zoomed only convar
static auto try_autoshoot(sdk::UserCmd *cmd) {
    auto autoshoot_allowed = false;

    // Only allow autoshoot when we are zoomed and can get headshots
    if (local_weapon->client_class()->class_id == class_id::CTFSniperRifle) {
        if ((local_player->cond() & 2)) {
            auto player_time = local_player->tick_base() * iface::globals->interval_per_tick;
            auto time_delta  = player_time - local_player->fov_time();

            if (time_delta >= 0.2) autoshoot_allowed = true;
        }
    } else {
        autoshoot_allowed = true;
    }

    if (autoshoot_allowed) cmd->buttons |= 1;

    return autoshoot_allowed;
}

static Convar<bool> doghook_aimbot_always_aim                   = Convar<bool>{"doghook_aimbot_always_aim", false, nullptr};
static Convar<bool> doghook_aimbot_silent                       = Convar<bool>{"doghook_aimbot_silent", true, nullptr};
static Convar<bool> doghook_aimbot_autoshoot                    = Convar<bool>{"doghook_aimbot_autoshoot", true, nullptr};
static Convar<bool> doghook_aimbot_aim_if_not_attack            = Convar<bool>{"doghook_aimbot_aim_if_not_attack", true, nullptr};
static Convar<bool> doghook_aimbot_disallow_attack_if_no_target = Convar<bool>{"doghook_aimbot_disallow_attack_if_no_target", false, nullptr};

void create_move(sdk::UserCmd *cmd) {
    profiler_profile_function();

    if (!can_find_targets) return;

    find_targets();

    if (doghook_aimbot_aim_if_not_attack != true) {
        // check if we are IN_ATTACK
        if ((cmd->buttons & 1) != 1) return;
    }

    if (targets.size() > 0 && targets[0].e != nullptr) {
        auto &target = targets[0];

        //iface::overlay->add_box_overlay(target.v, {-2, -2, -2}, {2, 2, 2}, {0, 0, 0}, 255, 255, 0, 100, 0);

        auto delta      = target.v - local_view;
        auto new_angles = delta.to_angle();
        new_angles      = clamp_angle(new_angles);

        // TODO: shouldnt this be on the outside instead
        if (local_weapon->can_shoot(local_player->tick_base())) {
            auto shot = (doghook_aimbot_autoshoot && try_autoshoot(cmd)) || (cmd->buttons & 1);

            if (shot || doghook_aimbot_always_aim) {
                auto new_movement = fix_movement_for_new_angles({cmd->forwardmove, cmd->sidemove, 0}, cmd->viewangles, new_angles);
                cmd->viewangles   = new_angles;

                cmd->forwardmove = new_movement.x;
                cmd->sidemove    = new_movement.y;

                cmd->tick_count -= target.cmd_delta;
            }
        }

        if (doghook_aimbot_silent == false) iface::engine->set_view_angles(new_angles);
    } else {
        if (doghook_aimbot_disallow_attack_if_no_target == true) cmd->buttons &= ~1;
    }
}

void create_move_pre_predict(sdk::UserCmd *cmd) {
    profiler_profile_function();
    // deal with some local data that we want to keep around
    local_player = Player::local();
    assert(local_player);

    if (local_player->alive() == false) {
        can_find_targets = false;
        return;
    }

    local_weapon = local_player->active_weapon()->to_weapon();

    local_team = local_player->team();
    local_view = local_player->view_position();

    // If we dont have the necessary information (we havent spawned yet or are dead)
    // then do not attempt to find targets.
    can_find_targets = local_weapon != nullptr;

    targets.clear();
    // TODO: should we reserve here?
}
} // namespace aimbot
