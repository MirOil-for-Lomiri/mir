/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2 or 3,
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
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "cursor.h"
#include "platform.h"
#include "kms_output.h"
#include "kms_output_container.h"
#include "kms_display_configuration.h"
#include "mir/geometry/rectangle.h"
#include "mir/graphics/cursor_image.h"

#include <xf86drm.h>
#include <drm/drm.h>

#include <boost/exception/errinfo_errno.hpp>

#include <stdexcept>
#include <vector>

namespace mg = mir::graphics;
namespace mgm = mg::mesa;
namespace geom = mir::geometry;

namespace
{
const uint64_t fallback_cursor_size = 64;
char const* const mir_drm_cursor_64x64 = "MIR_DRM_CURSOR_64x64";

// Transforms a relative position within the display bounds described by \a rect which is rotated with \a orientation
geom::Displacement transform(geom::Rectangle const& rect, geom::Displacement const& vector, MirOrientation orientation)
{
    switch(orientation)
    {
    case mir_orientation_left:
        return {vector.dy.as_int(), rect.size.width.as_int() -vector.dx.as_int()};
    case mir_orientation_inverted:
        return {rect.size.width.as_int() -vector.dx.as_int(), rect.size.height.as_int() - vector.dy.as_int()};
    case mir_orientation_right:
        return {rect.size.height.as_int() -vector.dy.as_int(), vector.dx.as_int()};
    default:
    case mir_orientation_normal:
        return vector;
    }
}
// support for older drm headers
#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH            0x8
#define DRM_CAP_CURSOR_HEIGHT           0x9
#endif

// In certain combinations of DRI backends and drivers GBM
// returns a stride size that matches the requested buffers size,
// instead of the underlying buffer:
// https://bugs.freedesktop.org/show_bug.cgi?id=89164
int get_drm_cursor_height(int fd)
{
    // on some older hardware drm incorrectly reports the cursor size
    bool const force_64x64_cursor = getenv(mir_drm_cursor_64x64);

    uint64_t height = fallback_cursor_size;
    if (!force_64x64_cursor)
       drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &height);
    return int(height);
}

int get_drm_cursor_width(int fd)
{
    // on some older hardware drm incorrectly reports the cursor size
    bool const force_64x64_cursor = getenv(mir_drm_cursor_64x64);

    uint64_t width = fallback_cursor_size;
    if (!force_64x64_cursor)
       drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &width);
    return int(width);
}

gbm_device* gbm_create_device_checked(int fd)
{
    auto device = gbm_create_device(fd);
    if (!device)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to create gbm device"));
    }
    return device;
}
}

mgm::Cursor::GBMBOWrapper::GBMBOWrapper(int fd) :
    device{gbm_create_device_checked(fd)},
    buffer{
        gbm_bo_create(
            device,
            get_drm_cursor_width(fd),
            get_drm_cursor_height(fd),
            GBM_FORMAT_ARGB8888,
            GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE)}
{
    if (!buffer) BOOST_THROW_EXCEPTION(std::runtime_error("failed to create gbm buffer"));
}

inline mgm::Cursor::GBMBOWrapper::operator gbm_bo*()
{
    return buffer;
}

inline mgm::Cursor::GBMBOWrapper::~GBMBOWrapper()
{
    if (device)
        gbm_device_destroy(device);
    if (buffer)
        gbm_bo_destroy(buffer);
}

mgm::Cursor::GBMBOWrapper::GBMBOWrapper(GBMBOWrapper&& from)
    : device{from.device},
      buffer{from.buffer}
{
    from.buffer = nullptr;
    from.device = nullptr;
}

mgm::Cursor::Cursor(
    KMSOutputContainer& output_container,
    std::shared_ptr<CurrentConfiguration> const& current_configuration) :
        output_container(output_container),
        current_position(),
        last_set_failed(false),
        min_buffer_width{std::numeric_limits<uint32_t>::max()},
        min_buffer_height{std::numeric_limits<uint32_t>::max()},
        current_configuration(current_configuration)
{
    // Generate the buffers for the initial configuration.
    current_configuration->with_current_configuration_do(
        [this](KMSDisplayConfiguration const& kms_conf)
        {
            kms_conf.for_each_output(
                [this, &kms_conf](auto const& output)
                {
                    // I'm not sure why g++ needs the explicit "this->" but it does - alan_g
                    this->buffer_for_output(*kms_conf.get_output_for(output.id));
                });
        });

    hide();
    if (last_set_failed)
        throw std::runtime_error("Initial KMS cursor set failed");
}

mgm::Cursor::~Cursor() noexcept
{
    hide();
}

void mgm::Cursor::write_buffer_data_locked(
    std::lock_guard<std::mutex> const&,
    gbm_bo* buffer,
    void const* data,
    size_t count)
{
    if (auto result = gbm_bo_write(buffer, data, count))
    {
        BOOST_THROW_EXCEPTION(
            ::boost::enable_error_info(std::runtime_error("failed to initialize gbm buffer"))
                << (boost::error_info<Cursor, decltype(result)>(result)));
    }
}

void mgm::Cursor::pad_and_write_image_data_locked(
    std::lock_guard<std::mutex> const& lg,
    gbm_bo* buffer,
    CursorImage const& image)
{
    auto image_argb = static_cast<uint8_t const*>(image.as_argb_8888());
    auto image_width = image.size().width.as_uint32_t();
    auto image_height = image.size().height.as_uint32_t();
    auto image_stride = image_width * 4;

    if (image_width > min_buffer_width || image_height > min_buffer_height)
    {
        BOOST_THROW_EXCEPTION(std::logic_error("Image is too big for GBM cursor buffer"));
    }

    size_t buffer_stride = gbm_bo_get_stride(buffer);  // in bytes
    size_t padded_size = buffer_stride * gbm_bo_get_height(buffer);
    auto padded = std::unique_ptr<uint8_t[]>(new uint8_t[padded_size]);
    size_t rhs_padding = buffer_stride - image_stride;

    uint8_t* dest = &padded[0];
    uint8_t const* src = image_argb;

    for (unsigned int y = 0; y < image_height; y++)
    {
        memcpy(dest, src, image_stride);
        memset(dest + image_stride, 0, rhs_padding);
        dest += buffer_stride;
        src += image_stride;
    }

    memset(dest, 0, buffer_stride * (gbm_bo_get_height(buffer) - image_height));

    write_buffer_data_locked(lg, buffer, &padded[0], padded_size);
}

void mgm::Cursor::show()
{
    std::lock_guard<std::mutex> lg(guard);

    if (!visible)
    {
        visible = true;
        place_cursor_at_locked(lg, current_position, ForceState);
    }
}

void mgm::Cursor::show(CursorImage const& cursor_image)
{
    std::lock_guard<std::mutex> lg(guard);

    auto const& size = cursor_image.size();

    hotspot = cursor_image.hotspot();
    {
        auto locked_buffers = buffers.lock();
        for (auto& pair : *locked_buffers)
        {
            auto& buffer = pair.second;
            if (size != geometry::Size{gbm_bo_get_width(buffer), gbm_bo_get_height(buffer)})
            {
                pad_and_write_image_data_locked(lg, buffer, cursor_image);
            }
            else
            {
                auto const count = size.width.as_uint32_t() * size.height.as_uint32_t() * sizeof(uint32_t);
                write_buffer_data_locked(lg, buffer, cursor_image.as_argb_8888(), count);
            }
        }
    }

    // Writing the data could throw an exception so lets
    // hold off on setting visible until after we have succeeded.
    visible = true;
    place_cursor_at_locked(lg, current_position, ForceState);
}

void mgm::Cursor::move_to(geometry::Point position)
{
    place_cursor_at(position, UpdateState);
}

void mir::graphics::mesa::Cursor::suspend()
{
    std::lock_guard<std::mutex> lg(guard);
    clear(lg);
}

void mir::graphics::mesa::Cursor::clear(std::lock_guard<std::mutex> const&)
{
    last_set_failed = false;
    output_container.for_each_output([&](std::shared_ptr<KMSOutput> const& output)
        {
            if (!output->clear_cursor())
                last_set_failed = true;
        });
}

void mgm::Cursor::resume()
{
    place_cursor_at(current_position, ForceState);
}

void mgm::Cursor::hide()
{
    std::lock_guard<std::mutex> lg(guard);
    visible = false;
    clear(lg);
}

void mgm::Cursor::for_each_used_output(
    std::function<void(KMSOutput&, geom::Rectangle const&, MirOrientation orientation)> const& f)
{
    current_configuration->with_current_configuration_do(
        [&f](KMSDisplayConfiguration const& kms_conf)
        {
            kms_conf.for_each_output([&](DisplayConfigurationOutput const& conf_output)
            {
                if (conf_output.used)
                {
                    auto output = kms_conf.get_output_for(conf_output.id);

                    f(*output, conf_output.extents(), conf_output.orientation);
                }
            });
        });
}

void mgm::Cursor::place_cursor_at(
    geometry::Point position,
    ForceCursorState force_state)
{
    std::lock_guard<std::mutex> lg(guard);
    place_cursor_at_locked(lg, position, force_state);
}

void mgm::Cursor::place_cursor_at_locked(
    std::lock_guard<std::mutex> const&,
    geometry::Point position,
    ForceCursorState force_state)
{

    current_position = position;

    if (!visible)
        return;

    bool set_on_all_outputs = true;

    for_each_used_output([&](KMSOutput& output, geom::Rectangle const& output_rect, MirOrientation orientation)
    {
        if (output_rect.contains(position))
        {
            auto dp = transform(output_rect, position - output_rect.top_left, orientation);

            // It's a little strange that we implement hotspot this way as there is
            // drmModeSetCursor2 with hotspot support. However it appears to not actually
            // work on radeon and intel. There also seems to be precedent in weston for
            // implementing hotspot in this fashion.
            output.move_cursor(geom::Point{} + dp - hotspot);
            if (force_state || !output.has_cursor()) // TODO - or if orientation had changed - then set buffer..
            {
                if (!output.set_cursor(buffer_for_output(output)) || !output.has_cursor())
                    set_on_all_outputs = false;
            }
        }
        else
        {
            if (force_state || output.has_cursor())
            {
                output.clear_cursor();
            }
        }
    });

    last_set_failed = !set_on_all_outputs;
}

gbm_bo* mgm::Cursor::buffer_for_output(KMSOutput const& output)
{
    auto locked_buffers = buffers.lock();

    auto buffer_it = std::find_if(
        locked_buffers->begin(),
        locked_buffers->end(),
        [&output](auto const& candidate)
        {
            return candidate.first == output.drm_fd();
        });

    if (buffer_it != locked_buffers->end())
    {
        return buffer_it->second;
    }

    locked_buffers->push_back(std::make_pair(output.drm_fd(), GBMBOWrapper(output.drm_fd())));

    gbm_bo* bo = locked_buffers->back().second;
    if (gbm_bo_get_width(bo) < min_buffer_width)
    {
        min_buffer_width = gbm_bo_get_width(bo);
    }
    if (gbm_bo_get_height(bo) < min_buffer_height)
    {
        min_buffer_height = gbm_bo_get_height(bo);
    }

    return bo;
}
