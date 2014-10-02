/*
 * Copyright © 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "mir/fd_socket_transmission.h"
#include "mir/variable_length_array.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <boost/throw_exception.hpp>
#include <boost/exception/errinfo_errno.hpp>
#include <stdexcept>
#include <mutex>

#include <unistd.h>
#include <fcntl.h>
       #include <sys/types.h>
       #include <sys/stat.h>

mir::socket_error::socket_error(std::string const& message) :
    std::system_error(errno, std::system_category(), message)
{
}

mir::socket_disconnected_error::socket_disconnected_error(std::string const& message) :
    std::system_error(errno, std::system_category(), message)
{
}

mir::fd_reception_error::fd_reception_error() :
    std::runtime_error("Invalid control message for receiving file descriptors")
{
}

void mir::send_fds(
    mir::Fd const& socket,
    std::vector<mir::Fd> const& fds)
{
    printf("SENDING %i\n", (int)fds.size());
    if (fds.size() > 0)
    {
        // We send dummy data
        struct iovec iov;
        char dummy_iov_data[2] = {'o', 'p'};
        iov.iov_base = &dummy_iov_data;
        iov.iov_len = 2;

        // Allocate space for control message
        static auto const builtin_n_fds = 5;
        static auto const builtin_cmsg_space = CMSG_SPACE(builtin_n_fds * sizeof(int));
        auto const fds_bytes = fds.size() * sizeof(int);
        mir::VariableLengthArray<builtin_cmsg_space> control{CMSG_SPACE(fds_bytes)};
        // Silence valgrind uninitialized memory complaint
        memset(control.data(), 0, control.size());

        // Message to send
        struct msghdr header;
        header.msg_name = NULL;
        header.msg_namelen = 0;
        header.msg_iov = &iov;
        header.msg_iovlen = 1;
        header.msg_controllen = control.size();
        header.msg_control = control.data();
        header.msg_flags = 0;

        // Control message contains file descriptors
        struct cmsghdr *message = CMSG_FIRSTHDR(&header);
        message->cmsg_len = CMSG_LEN(fds_bytes);
        message->cmsg_level = SOL_SOCKET;
        message->cmsg_type = SCM_RIGHTS;

        int* const data = reinterpret_cast<int*>(CMSG_DATA(message));
        int i = 0;
        for (auto& fd : fds)
            data[i++] = fd;


        printf("SEND! erron %i\n", errno);
        auto const sent = sendmsg(socket, &header, 0);//MSG_DONTWAIT);
        printf("SENDing DONEE! %i %i\n", (int) sent, errno);
        if (sent < 0)
            BOOST_THROW_EXCEPTION(std::runtime_error("Failed to send fds: " + std::string(strerror(errno))));
    }
}

bool mir::socket_error_is_transient(int error_code)
{
    return (error_code == EINTR);
}

void mir::receive_data(mir::Fd const& socket, void* buffer, size_t bytes_requested, std::vector<mir::Fd>& fds)
{
    if (bytes_requested == 0)
        BOOST_THROW_EXCEPTION(std::logic_error("Attempted to receive 0 bytes"));

    printf("EXPECTING %i\n", (int) fds.size());
    size_t bytes_read{0};
    unsigned fds_read{0};
    while (bytes_read < bytes_requested)
    {
        // Store the data in the buffer requested
        struct iovec iov;
        iov.iov_base = static_cast<uint8_t*>(buffer) + bytes_read;
        iov.iov_len = bytes_requested - bytes_read;
        
        // Allocate space for control message
        static auto const builtin_n_fds = 5;
        static auto const builtin_cmsg_space = CMSG_SPACE(builtin_n_fds * sizeof(int));
        auto fds_bytes = (fds.size() - fds_read) * sizeof(int);
        mir::VariableLengthArray<builtin_cmsg_space> control{CMSG_SPACE(fds_bytes)};
       
//        printf("CONTROL SIZE %i %i %i %i\n", (int) CMSG_LEN(fds_bytes),(int) CMSG_SPACE(fds_bytes), (int) builtin_cmsg_space, (int) control.size()); 
        // Message to read
        struct msghdr header;
        header.msg_name = NULL;
        header.msg_namelen = 0;
        header.msg_iov = &iov;
        header.msg_iovlen = 1;
 //       printf("MSG %i %i\n", (int) control.size(), (int) CMSG_LEN(control.size()));
        header.msg_controllen = control.size();
        header.msg_control = control.data();
        header.msg_flags = 0;

        auto fl = fcntl(socket, F_GETFL, 0);
        fl = fl & ~O_NONBLOCK;
        fcntl(socket, F_SETFL, fl);
        printf("WAIIIIT------------------------------>\n");
        ssize_t result = recvmsg(socket, &header, MSG_NOSIGNAL | MSG_WAITALL);
        printf("done wAIIIIT------------------------------>\n");
        if (result == 0)
            BOOST_THROW_EXCEPTION(socket_disconnected_error("Failed to read message from server: server has shutdown"));
                
        if (result < 0)
        {
            if (socket_error_is_transient(errno))
                continue;
            if (errno == EPIPE)
                BOOST_THROW_EXCEPTION(
                    boost::enable_error_info(socket_disconnected_error("Failed to read message from server PIPE"))
                << boost::errinfo_errno(errno));

            BOOST_THROW_EXCEPTION(
                        boost::enable_error_info(socket_error("Failed to read message from server"))
                        << boost::errinfo_errno(errno));
        }

        bytes_read += result;
       
        // If we get a proper control message, copy the received
        // file descriptors back to the caller
        struct cmsghdr * cmsg = CMSG_FIRSTHDR(&header);
        if (cmsg)
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS)
            {
                printf("CREDENTIAL\n");
//                BOOST_THROW_EXCEPTION(std::runtime_error("got a credential instead of a fd"));
            }
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            {
                printf("%X %X %X LEVEL 0x%X TYPE 0x%X\n", SOL_SOCKET, SCM_RIGHTS, SCM_CREDENTIALS, cmsg->cmsg_level, cmsg->cmsg_type);
                BOOST_THROW_EXCEPTION(fd_reception_error());
            }

            // NOTE: This relies on the file descriptor cmsg being read
            // (and written) atomically.
            if (cmsg->cmsg_len > CMSG_LEN(fds_bytes) || (header.msg_flags & MSG_CTRUNC))
            {
                printf("ERROR (%X) %i --- %i\n", header.msg_flags,(int) cmsg->cmsg_len,(int) CMSG_LEN(fds_bytes));
                BOOST_THROW_EXCEPTION(std::runtime_error("Received more fds than expected"));
            }
            int const* const data = reinterpret_cast<int const*>CMSG_DATA(cmsg);
            ptrdiff_t const header_size = reinterpret_cast<char const*>(data) - reinterpret_cast<char const*>(cmsg);
            int const nfds = (cmsg->cmsg_len - header_size) / sizeof(int);

            // We can't properly pass mir::Fds through google::protobuf::Message,
            // which is where these get shoved.
            //
            // When we have our own RPC generator plugin and aren't using deprecated
            // Protobuf features this can go away.
            for (int i = 0; i < nfds; i++)
            {
                fds[fds_read + i] = mir::Fd{mir::IntOwnedFd{data[i]}};
                printf("--->VALID?? %i\n", fcntl(fds[fds_read+i], F_GETFD));
            }

            fds_read += nfds;
        }
        printf("BYTES GOT %i %i\n",(int)bytes_read,(int)bytes_requested);
    }

    printf("AND EXIT.\n");
    if (fds_read < fds.size())
        BOOST_THROW_EXCEPTION(std::runtime_error("Receieved fewer fds than expected"));
}
