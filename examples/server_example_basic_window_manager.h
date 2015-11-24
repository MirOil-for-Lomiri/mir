/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored By: Alan Griffiths <alan@octopull.co.uk>
 */

#ifndef MIR_EXAMPLE_BASIC_WINDOW_MANAGER_H_
#define MIR_EXAMPLE_BASIC_WINDOW_MANAGER_H_

#include "server_example_canonical_surface_info.h"

#include "mir/geometry/rectangles.h"
#include "mir/scene/session.h"
#include "mir/scene/surface.h"
#include "mir/scene/surface_creation_parameters.h"
#include "mir/shell/abstract_shell.h"
#include "mir/shell/window_manager.h"

#include <map>
#include <mutex>

///\example server_example_basic_window_manager.h
/// A generic policy-based window manager implementation

namespace mir
{
namespace examples
{
using shell::SurfaceSet;

/// The interface through which the policy instructs the controller.
/// These functions assume that the BasicWindowManager data structures can be accessed freely.
/// I.e. should only be invoked by the policy handle_... methods (where any necessary locks are held).
// TODO extract commonality with FocusController (once that's separated from shell::FocusController)
template<typename SessionInfo>
class BasicWindowManagerToolsCopy
{
public:
    using SurfaceInfo = CanonicalSurfaceInfoCopy;
    using SurfaceInfoMap = std::map<std::weak_ptr<scene::Surface>, SurfaceInfo, std::owner_less<std::weak_ptr<scene::Surface>>>;
    using SessionInfoMap = std::map<std::weak_ptr<scene::Session>, SessionInfo, std::owner_less<std::weak_ptr<scene::Session>>>;

    virtual auto find_session(std::function<bool(SessionInfo const& info)> const& predicate)
    -> std::shared_ptr<scene::Session> = 0;

    virtual auto info_for(std::weak_ptr<scene::Session> const& session) const -> SessionInfo& = 0;

    virtual auto info_for(std::weak_ptr<scene::Surface> const& surface) const -> SurfaceInfo& = 0;

    virtual std::shared_ptr<scene::Session> focused_session() const = 0;

    virtual std::shared_ptr<scene::Surface> focused_surface() const = 0;

    virtual void focus_next_session() = 0;

    virtual void set_focus_to(
        std::shared_ptr<scene::Session> const& focus,
        std::shared_ptr<scene::Surface> const& surface) = 0;

    virtual auto surface_at(geometry::Point cursor) const -> std::shared_ptr<scene::Surface> = 0;

    virtual auto active_display() -> geometry::Rectangle const = 0;

    virtual void forget(std::weak_ptr<scene::Surface> const& surface) = 0;

    virtual void raise_tree(std::shared_ptr<scene::Surface> const& root) = 0;

    virtual ~BasicWindowManagerToolsCopy() = default;
    BasicWindowManagerToolsCopy() = default;
    BasicWindowManagerToolsCopy(BasicWindowManagerToolsCopy const&) = delete;
    BasicWindowManagerToolsCopy& operator=(BasicWindowManagerToolsCopy const&) = delete;
};

/// A policy based window manager.
/// This takes care of the management of any meta implementation held for the sessions and surfaces.
///
/// \tparam WindowManagementPolicy the constructor must take a pointer to BasicWindowManagerTools<>
/// as its first parameter. (Any additional parameters can be forwarded by
/// BasicWindowManager::BasicWindowManager.)
/// In addition WindowManagementPolicy must implement the following methods:
/// - void handle_session_info_updated(SessionInfoMap& session_info, Rectangles const& displays);
/// - void handle_displays_updated(SessionInfoMap& session_info, Rectangles const& displays);
/// - auto handle_place_new_surface(std::shared_ptr<ms::Session> const& session, ms::SurfaceCreationParameters const& request_parameters) -> ms::SurfaceCreationParameters;
/// - void handle_new_surface(std::shared_ptr<ms::Session> const& session, std::shared_ptr<ms::Surface> const& surface);
/// - void handle_delete_surface(std::shared_ptr<ms::Session> const& /*session*/, std::weak_ptr<ms::Surface> const& /*surface*/);
/// - int handle_set_state(std::shared_ptr<ms::Surface> const& surface, MirSurfaceState value);
/// - bool handle_keyboard_event(MirKeyboardEvent const* event);
/// - bool handle_touch_event(MirTouchEvent const* event);
/// - bool handle_pointer_event(MirPointerEvent const* event);
///
/// \tparam SessionInfo must be default constructable.
///
/// \tparam SurfaceInfo must be constructable from (std::shared_ptr<ms::Session>, std::shared_ptr<ms::Surface>, ms::SurfaceCreationParameters const& params)
template<typename WindowManagementPolicy, typename SessionInfo>
class BasicWindowManagerCopy : public shell::WindowManager,
    private BasicWindowManagerToolsCopy<SessionInfo>
{
public:
    using typename BasicWindowManagerToolsCopy<SessionInfo>::SurfaceInfo;
    using typename BasicWindowManagerToolsCopy<SessionInfo>::SurfaceInfoMap;
    using typename BasicWindowManagerToolsCopy<SessionInfo>::SessionInfoMap;

    template <typename... PolicyArgs>
    BasicWindowManagerCopy(
        shell::FocusController* focus_controller,
        PolicyArgs&&... policy_args) :
        focus_controller(focus_controller),
        policy(this, std::forward<PolicyArgs>(policy_args)...)
    {
    }

private:
    void add_session(std::shared_ptr<scene::Session> const& session) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        session_info[session] = SessionInfo();
        policy.handle_session_info_updated(session_info, displays);
    }

    void remove_session(std::shared_ptr<scene::Session> const& session) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        session_info.erase(session);
        policy.handle_session_info_updated(session_info, displays);
    }

    frontend::SurfaceId add_surface(
        std::shared_ptr<scene::Session> const& session,
        scene::SurfaceCreationParameters const& params,
        std::function<frontend::SurfaceId(std::shared_ptr<scene::Session> const& session, scene::SurfaceCreationParameters const& params)> const& build) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        scene::SurfaceCreationParameters const placed_params = policy.handle_place_new_surface(session, params);
        auto const result = build(session, placed_params);
        auto const surface = session->surface(result);
        surface_info.emplace(surface, SurfaceInfo{session, surface, placed_params});
        policy.handle_new_surface(session, surface);
        policy.generate_decorations_for(session, surface, surface_info, build);
        return result;
    }

    void modify_surface(
        std::shared_ptr<scene::Session> const& session,
        std::shared_ptr<scene::Surface> const& surface,
        shell::SurfaceSpecification const& modifications) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        policy.handle_modify_surface(session, surface, modifications);
    }

    void remove_surface(
        std::shared_ptr<scene::Session> const& session,
        std::weak_ptr<scene::Surface> const& surface) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        policy.handle_delete_surface(session, surface);

        surface_info.erase(surface);
    }

    void forget(std::weak_ptr<scene::Surface> const& surface) override
    {
        surface_info.erase(surface);
    }

    void add_display(geometry::Rectangle const& area) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        displays.add(area);
        policy.handle_displays_updated(session_info, displays);
    }

    void remove_display(geometry::Rectangle const& area) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        displays.remove(area);
        policy.handle_displays_updated(session_info, displays);
    }

    bool handle_keyboard_event(MirKeyboardEvent const* event) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        update_event_timestamp(event);
        return policy.handle_keyboard_event(event);
    }

    bool handle_touch_event(MirTouchEvent const* event) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        update_event_timestamp(event);
        return policy.handle_touch_event(event);
    }

    bool handle_pointer_event(MirPointerEvent const* event) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        update_event_timestamp(event);

        cursor = {
            mir_pointer_event_axis_value(event, mir_pointer_axis_x),
            mir_pointer_event_axis_value(event, mir_pointer_axis_y)};

        return policy.handle_pointer_event(event);
    }

    void handle_raise_surface(
        std::shared_ptr<scene::Session> const& session,
        std::shared_ptr<scene::Surface> const& surface,
        uint64_t timestamp) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        if (timestamp >= last_input_event_timestamp)
            policy.handle_raise_surface(session, surface);
    }

    int set_surface_attribute(
        std::shared_ptr<scene::Session> const& /*session*/,
        std::shared_ptr<scene::Surface> const& surface,
        MirSurfaceAttrib attrib,
        int value) override
    {
        std::lock_guard<decltype(mutex)> lock(mutex);
        switch (attrib)
        {
        case mir_surface_attrib_state:
        {
            auto const state = policy.handle_set_state(surface, MirSurfaceState(value));
            return surface->configure(attrib, state);
        }
        default:
            return surface->configure(attrib, value);
        }
    }

    auto find_session(std::function<bool(SessionInfo const& info)> const& predicate)
    -> std::shared_ptr<scene::Session> override
    {
        for(auto& info : session_info)
        {
            if (predicate(info.second))
            {
                return info.first.lock();
            }
        }

        return std::shared_ptr<scene::Session>{};
    }

    auto info_for(std::weak_ptr<scene::Session> const& session) const -> SessionInfo& override
    {
        return const_cast<SessionInfo&>(session_info.at(session));
    }

    auto info_for(std::weak_ptr<scene::Surface> const& surface) const -> SurfaceInfo& override
    {
        return const_cast<SurfaceInfo&>(surface_info.at(surface));
    }

    std::shared_ptr<scene::Session> focused_session() const override
    {
        return focus_controller->focused_session();
    }

    std::shared_ptr<scene::Surface> focused_surface() const override
    {
        return focus_controller->focused_surface();
    }

    void focus_next_session() override
    {
        focus_controller->focus_next_session();
    }

    void set_focus_to(
        std::shared_ptr<scene::Session> const& focus,
        std::shared_ptr<scene::Surface> const& surface) override
    {
        focus_controller->set_focus_to(focus, surface);
    }

    auto surface_at(geometry::Point cursor) const -> std::shared_ptr<scene::Surface> override
    {
        return focus_controller->surface_at(cursor);
    }

    auto active_display() -> geometry::Rectangle const override
    {
        geometry::Rectangle result;

        // 1. If a window has input focus, whichever display contains the largest
        //    proportion of the area of that window.
        if (auto const surface = focused_surface())
        {
            auto const surface_rect = surface->input_bounds();
            int max_overlap_area = -1;

            for (auto const& display : displays)
            {
                auto const intersection = surface_rect.intersection_with(display).size;
                if (intersection.width.as_int()*intersection.height.as_int() > max_overlap_area)
                {
                    max_overlap_area = intersection.width.as_int()*intersection.height.as_int();
                    result = display;
                }
            }
            return result;
        }

        // 2. Otherwise, if any window previously had input focus, for the window that had
        //    it most recently, the display that contained the largest proportion of the
        //    area of that window at the moment it closed, as long as that display is still
        //    available.

        // 3. Otherwise, the display that contains the pointer, if there is one.
        for (auto const& display : displays)
        {
            if (display.contains(cursor))
            {
                // Ignore the (unspecified) possiblity of overlapping displays
                return display;
            }
        }

        // 4. Otherwise, the primary display, if there is one (for example, the laptop display).

        // 5. Otherwise, the first display.
        if (displays.size())
            result = *displays.begin();

        return result;
    }

    void raise_tree(std::shared_ptr<scene::Surface> const& root) override
    {
        SurfaceSet surfaces;
        std::function<void(std::weak_ptr<scene::Surface> const& surface)> const add_children =
            [&,this](std::weak_ptr<scene::Surface> const& surface)
                {
                auto const& info = info_for(surface);
                surfaces.insert(begin(info.children), end(info.children));
                for (auto const& child : info.children)
                    add_children(child);
                };

        surfaces.insert(root);
        add_children(root);

        focus_controller->raise(surfaces);
    }

    shell::FocusController* const focus_controller;
    WindowManagementPolicy policy;

    std::mutex mutex;
    SessionInfoMap session_info;
    SurfaceInfoMap surface_info;
    geometry::Rectangles displays;
    geometry::Point cursor;
    uint64_t last_input_event_timestamp{0};

private:
    void update_event_timestamp(MirKeyboardEvent const* kev)
    {
        auto iev = mir_keyboard_event_input_event(kev);
        last_input_event_timestamp = mir_input_event_get_event_time(iev);
    }

    void update_event_timestamp(MirPointerEvent const* pev)
    {
        auto iev = mir_pointer_event_input_event(pev);
        auto pointer_action = mir_pointer_event_action(pev);

        if (pointer_action == mir_pointer_action_button_up ||
            pointer_action == mir_pointer_action_button_down)
        {
            last_input_event_timestamp = mir_input_event_get_event_time(iev);
        }
    }

    void update_event_timestamp(MirTouchEvent const* tev)
    {
        auto iev = mir_touch_event_input_event(tev);
        auto touch_count = mir_touch_event_point_count(tev);
        for (unsigned i = 0; i < touch_count; i++)
        {
            auto touch_action = mir_touch_event_action(tev, i);
            if (touch_action == mir_touch_action_up ||
                   touch_action == mir_touch_action_down)
            {
                last_input_event_timestamp = mir_input_event_get_event_time(iev);
                break;
            }
        }
    }
};
}
}

#endif /* MIR_EXAMPLE_BASIC_WINDOW_MANAGER_H_ */
