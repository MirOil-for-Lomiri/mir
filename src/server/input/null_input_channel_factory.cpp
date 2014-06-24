/*
 * Copyright © 2014 Canonical Ltd.
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
 * Authored by: Andreas Pokorny <andreas.pokorny@canonical.com>
 */

#include "null_input_channel_factory.h"

#include "mir/input/input_channel.h"

namespace mi = mir::input;

namespace
{
class NullInputChannel : public mi::InputChannel
{
    int client_fd() const override
    {
        return 0;
    }
    int server_fd() const override
    {
        return 0;
    }
};
}

std::shared_ptr<mi::InputChannel> mi::NullInputChannelFactory::make_input_channel()
{
    return std::make_shared<NullInputChannel>();
}

