--[[
    Widget for AwesomeWM 4 to display the stream of term_pa_spectrum.

    To use it:

    local term_pa_spectrum = require("term_pa_spectrum.awm_widget")

    and include the widget in some wibar:

    local widget_for_mywibox = term_pa_spectrum(s.mywibox),

    Version: 1.0.0
    Author: Jose Maria Perez Ramos <jose.m.perez.ramos+git gmail>
    Date: 2018.08.25

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
]]

-- Since having a textbox that continously reads the output of a running
-- program it's quite expensive, this widget creates a static widget and
-- places a terminal (I've tested urxvt) over it, maintaining its position.

local _selfname, selfpath = ...
local awful  = require("awful")
local timer  = require("gears.timer")
local wibox = require("wibox")

local command_path = (selfpath and selfpath:match("(.*)/[^/]*.lua") or "").."/../term_pa_spectrum"
if not require("gears.filesystem").file_readable(command_path) then
    require("naughty").notify{
        title = "Error while launching term_pa_spectrum",
        text = "term_pa_spectrum executable not found, have you run 'make'?",
        timeout = 0,
    }
    return function() end
end

local widget = {
    started = false,
    client = nil,
    snid = nil,
    placeholders = {},
    holder = nil,
}

local options = {
    terminal = (terminal or "urxvt") .. " -e ",
    command = command_path,
    args = {
        ["-c"] = "braille",
        ["-b"] = 30,
        ["-g"] = "lineal",
        ["-G"] = "avg",
    },
    width = 128,
    height = 16,
    periodic_check_timer = 60,
}

function widget:add_placeholder(placeholder_wibar, provided_options)
    if not placeholder_wibar then return end

    self:init(provided_options)

    local placeholder = wibox.widget{
        wibar = placeholder_wibar,
        index = #self.placeholders,
        fit = function(self, context, width, height)
            local w_slack = 0
            if self == widget.holder and placeholder_wibar.visible then
                self.width, self.height = math.min(width, options.width), math.min(height, options.height)
                w_slack = 2
            else
                self.width, self.height = 0, 0
            end
            return self.width + w_slack, self.height
        end,
        draw = function(self, context, cairo, width, height)
            widget:trigger_resize(self, context.wibox, self.width, self.height)
        end,
    }
    table.insert(self.placeholders, placeholder)

    placeholder_wibar:connect_signal("property::screen",  function() self:choose_screen() end)
    placeholder_wibar:connect_signal("property::visible", function()
        if placeholder == self.holder and self.client then
            self.client.hidden = not placeholder_wibar.visible
        end
    end)
    self:choose_screen()

    return placeholder
end

screen.connect_signal("removed", function() widget:choose_screen() end)

function widget:spawn()
    -- $ tput civis # disable terminal cursor
    local command = options.terminal .. " bash -c 'tput civis && " .. options.command.."'"
    self.snid = awful.spawn(command, {
        titlebars_enabled = false
    }, function(c) -- Triggers before "manage" event
        if self.client then
            c:kill()
        else
            self.client = c;
            c.hidden = true
        end
    end)
end

-- Create the terminal display
function widget:init(provided_options)
    if self.started then return end
    self.started = true

    options = setmetatable(provided_options or {}, { __index = options })

    for k,v in pairs(options.args or {}) do
        options.command = options.command.." "..k
        if v and type(v) ~= "boolean" then
            options.command = options.command.." "..v
        end
    end

    client.connect_signal("manage", function(c)
        if c and c == self.client then
            c:connect_signal("unmanage", function()
                self.client = nil
                self.snid = nil
                timer.start_new(options.periodic_check_timer, function() self:spawn() end)
            end)

            c:connect_signal("property::focusable", function()
                c.focusable = false
            end)

            c.below = true
            c.skip_taskbar = true
            c.sticky = true
            c.urgent = false
            c.ontop = false
            c.focusable = false
            c.border_width = 0
            c.floating = true
            c.size_hints_honor = false
            c:buttons{}
            c:keys{}
            c.is_fixed = true
            awful.titlebar.hide(c)

            local no_tags = function() c:tags{} end
            c:connect_signal("tagged", no_tags)
            timer.delayed_call(no_tags)

            if self.holder then
                self.holder:emit_signal("widget::layout_changed")
            end
        end
    end)

    awesome.connect_signal("exit", function()
        if self.client then
            self.client:kill()
        end
    end)

    awful.ewmh.add_activate_filter(function(c)
        if c and c == self.client then
            return false
        end
    end)

    self:spawn()

    timer.start_new(options.periodic_check_timer, function()
        if self.holder then
            self.holder:emit_signal("widget::layout_changed")
        end
        return true
    end)
end

do
    local function throw_widget_hierarchy(hierarchy, widget)
        if hierarchy:get_widget() == widget then
            error(hierarchy)
        end
        for _, child_hierarchy in pairs(hierarchy:get_children()) do
            throw_widget_hierarchy(child_hierarchy, widget)
        end
    end

    local function widget_corner_in_wibox(widget, wibox)
        local base_row = wibox:find_widgets(0,0)[1]
        if not base_row then return end

        -- Yeah, I know this is not the intended use of error and pcall
        local _, hierarchy = pcall(function() throw_widget_hierarchy(base_row.hierarchy, widget) end)
        if not hierarchy then return end

        -- The hierarchy could be cached if we had some way to invalidate the cache
        return hierarchy:get_matrix_to_device():transform_point(0, 0)
    end

    function widget:trigger_resize(placeholder, wibox, width, height)
        if width > 0 and height > 0 then
            local x, y = widget_corner_in_wibox(placeholder, wibox)
            if x then
                self.client:geometry{
                    x = x + wibox.x,
                    y = y + wibox.y,
                    height = height,
                    width = width,
                }
                self.client.hidden = false
                return
            end
        end
        self.client.hidden = true
    end
end

function widget:choose_screen()
    local previous_holder = self.holder

    -- Remove placeholders that have no screen to display
    self.holder = nil;
    local screens_by_id = {}
    for s in screen do
        screens_by_id[s] = true
    end

    local placeholders_keys_to_remove = {}
    for key, placeholder in pairs(self.placeholders) do
        if not placeholder.wibar.screen or not screens_by_id[placeholder.wibar.screen] then
            table.insert(placeholders_keys_to_remove, key)
        end
    end
    for _, key in pairs(placeholders_keys_to_remove) do
        self.placeholders[key] = nil
    end

    -- From the remaining ones, choose the best one
    local max_resolution = 0;
    local max_size = 0;
    for _, placeholder in pairs(self.placeholders) do
        local s = placeholder.wibar.screen
        local resolution = s.geometry.width * s.geometry.height
        local _name, values = next(s.outputs)
        local size = values.mm_width * values.mm_height
        if resolution > max_resolution or (resolution == max_resolution and size > max_size) then
            max_resolution = resolution
            max_size = size
            self.holder = placeholder
        end
    end

    if self.holder ~= previous_holder then
        if self.holder then
            self.holder:emit_signal("widget::layout_changed")
        end
        if previous_holder then
            previous_holder:emit_signal("widget::layout_changed")
        end
    end
end

function widget:is_widget_terminal(c) return c and c == self.client end

return setmetatable(widget, { __call = function(t, ...) return t:add_placeholder(...) end })

