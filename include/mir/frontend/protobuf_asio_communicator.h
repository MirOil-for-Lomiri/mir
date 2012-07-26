/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Guest <thomas.guest@canonical.com>
 */

#ifndef MIR_FRONTEND_PROTOBUF_ASIO_COMMUNICATOR_H_
#define MIR_FRONTEND_PROTOBUF_ASIO_COMMUNICATOR_H_

#include "communicator.h"

#include <string>

namespace mir
{
namespace frontend
{

class ProtobufAsioCommunicator : public Communicator
{
public:
    // Create communicator based on Boost asio and Google protobufs
    // using the supplied socket.
    ProtobufAsioCommunicator(std::string const& socket_file);

    ~ProtobufAsioCommunicator();
private:
    std::string const socket_file;
};

}
}

#endif // MIR_FRONTEND_PROTOBUF_ASIO_COMMUNICATOR_H_
